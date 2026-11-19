"""Frame-local CPU trees and source-frame-aware native renderer observations."""
from collections import defaultdict
import json
import math


def start(row):
    return int(row["ns_since_start"])


def end(row):
    return start(row) + int(row["exec_time_ns"])


def identity(row):
    return row["name"], row["src_file"], row["src_line"], row["thread"], start(row)


def interval_union(intervals):
    total = 0
    last = None
    for first, stop in sorted(intervals):
        if stop < first:
            raise ValueError("A timing interval ends before it begins")
        if last is None or first > last:
            total += stop - first
        elif stop > last:
            total += stop - last
        last = stop if last is None else max(last, stop)
    return total


def waiting(name):
    value = name.lower()
    return "wait" in value or "sleep" in value or "acquire drawable" in value


def frame_breakdown(events, self_events, frame_zone, window_ns):
    own = {identity(row): int(row["exec_time_ns"]) for row in self_events if row["exec_time_ns"]}
    roots = [row for row in events if row["name"] == frame_zone]
    threads = sorted({row["thread"] for row in roots})
    nodes = {}
    root_keys = []
    for thread in threads:
        stack = []
        for row in sorted((r for r in events if r["thread"] == thread),
                          key=lambda r: (start(r), -end(r), r["name"] != frame_zone)):
            first, stop = start(row), end(row)
            while stack and first >= stack[-1][0]:
                stack.pop()
            if stack and stop > stack[-1][0]:
                raise ValueError("CPU timing scopes cross on one thread")
            if row["name"] == frame_zone:
                if stack:
                    raise ValueError("Host frame zones overlap on the same thread")
                path = ((row["name"], row["src_file"], row["src_line"], thread),)
                if path not in root_keys:
                    root_keys.append(path)
            elif stack:
                path = stack[-1][1] + ((row["name"], row["src_file"], row["src_line"], thread),)
            else:
                continue
            value = nodes.setdefault(path, {"name": row["name"], "source": row["src_file"],
                "line": int(row["src_line"]), "thread": thread, "calls": 0,
                "inclusive_ns": 0, "self_ns": 0})
            value["calls"] += 1
            value["inclusive_ns"] += stop - first
            self_ns = own.get(identity(row))
            if self_ns is None or not 0 <= self_ns <= stop - first:
                raise ValueError("A CPU scope lacks a matching valid self duration")
            value["self_ns"] += self_ns
            stack.append((stop, path))
    count = len(roots)
    children = defaultdict(list)
    for path in nodes:
        if len(path) > 1:
            children[path[:-1]].append(path)

    def build(path):
        value = nodes[path]
        return {**value, "per_frame_ms": value["inclusive_ns"] / max(1, count) / 1e6,
                "self_per_frame_ms": value["self_ns"] / max(1, count) / 1e6,
                "wait_related": waiting(value["name"]),
                "children": [build(key) for key in sorted(children[path],
                    key=lambda key: nodes[key]["inclusive_ns"], reverse=True)]}

    total = sum(nodes[key]["inclusive_ns"] for key in root_keys)
    root_self = sum(nodes[key]["self_ns"] for key in root_keys)
    thread_rows = []
    for thread in sorted({row["thread"] for row in events}):
        rows = [row for row in events if row["thread"] == thread]
        covered = interval_union((start(row), end(row)) for row in rows)
        waits = interval_union((start(row), end(row)) for row in rows if waiting(row["name"]))
        thread_rows.append({"thread": thread, "main": thread in threads,
            "covered_ms": covered / 1e6, "wait_related_ms": waits / 1e6,
            "covered_window_percent": covered / window_ns * 100 if window_ns > 0 else None,
            "completed_zones": len(rows)})
    return {"frames": count, "main_threads": threads,
        "direct_stage_coverage_percent": (1 - root_self / total) * 100 if total else None,
        "roots": [build(key) for key in root_keys], "threads": thread_rows}


def native_view_timings(messages, first, last, statistics_of):
    records = {"cpuView": {}, "gpuView": {}, "gpuFrame": {}}
    for row in messages:
        if not first <= int(row["total_ns"]) < last:
            continue
        message = row["MessageName"]
        if message == "Bgfx.profilerStackImbalance":
            raise ValueError("The bgfx profiler emitted unbalanced begin/end callbacks")
        for kind in records:
            prefix = "Render." + kind + " "
            if not message.startswith(prefix):
                continue
            value = json.loads(message[len(prefix):])
            begin, stop, frequency = value["begin"], value["end"], value["frequency"]
            if frequency <= 0 or begin <= 0 or stop < begin:
                raise ValueError("A native view timestamp is invalid")
            value["duration_ms"] = (stop - begin) * 1000 / frequency
            if not math.isfinite(value["duration_ms"]):
                raise ValueError("A native view duration is not finite")
            key = value["viewId"], value["sourceFrame"], begin, stop, frequency
            records[kind][key] = value
            break
    views = {}
    for kind in ["cpuView", "gpuView"]:
        for value in records[kind].values():
            view = views.setdefault(value["viewId"], {"view_id": value["viewId"],
                "name": value["name"], "group": value["group"], "cpu_ms": [],
                "gpu_ms": [], "source_frames": set()})
            view["cpu_ms" if kind == "cpuView" else "gpu_ms"].append(value["duration_ms"])
            if kind == "gpuView":
                view["source_frames"].add(value["sourceFrame"])
    gpu_frames = defaultdict(list)
    for value in records["gpuView"].values():
        gpu_frames[value["sourceFrame"]].append(value)
    unions = []
    group_spans = defaultdict(list)
    for frame, rows in sorted(gpu_frames.items()):
        frequencies = {row["frequency"] for row in rows}
        if len(frequencies) != 1:
            raise ValueError("Native view clocks differ within one GPU source frame")
        frequency = next(iter(frequencies))
        intervals = defaultdict(list)
        for row in rows:
            intervals[row["group"]].append((row["begin"], row["end"]))
        for group, spans in intervals.items():
            group_spans[group].append(interval_union(spans) * 1000 / frequency)
        union_ms = interval_union((row["begin"], row["end"]) for row in rows) * 1000 / frequency
        total_ms = sum(row["duration_ms"] for row in rows)
        unions.append({"source_frame": frame, "observed_views": len(rows), "union_ms": union_ms,
            "sum_ms": total_ms, "overlap_ms": max(0.0, total_ms - union_ms)})
    normalized = []
    for view in sorted(views.values(), key=lambda value: value["view_id"]):
        frames = sorted(view.pop("source_frames"))
        normalized.append({**view, "cpu_ms": statistics_of(view["cpu_ms"]),
            "gpu_ms": statistics_of(view["gpu_ms"]), "gpu_source_frames": frames,
            "gpu_available": bool(view["gpu_ms"])})
    return {"views": normalized, "gpu_view_frames": unions,
        "gpu_view_frame_count": len(gpu_frames),
        "gpu_group_spans": [{"group": group, "gpu_ms": statistics_of(spans)}
            for group, spans in sorted(group_spans.items())],
        "gpu_view_union_ms": statistics_of([row["union_ms"] for row in unions]),
        "gpu_view_overlap_ms": statistics_of([row["overlap_ms"] for row in unions]),
        "missing_gpu_view_ids": [view["view_id"] for view in normalized if not view["gpu_available"]],
        "gpu_command_buffer_ms": statistics_of([value["duration_ms"]
            for value in records["gpuFrame"].values()]),
        "raw_unique_records": {kind: len(values) for kind, values in records.items()}}
