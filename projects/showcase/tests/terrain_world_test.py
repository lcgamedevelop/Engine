#!/usr/bin/env python3
"""Exercise terrain streaming, RVT and checkpoint requests through the existing MCP session."""
import argparse
from datetime import datetime, timezone
import json
from pathlib import Path
import uuid

from mcp_interaction_test import (PROJECT, EditorMcpSession, click, execute, lifecycle,
                                  require, wait_for)

READ = """
export default function(_args: unknown, gameplay: unknown) {
    const menu = gameplay as { model: { selected: {id:string}|null; error:string|null };
        active: { model: {busy:boolean;status:string} }|null };
    return {selected:menu.model.selected?.id ?? null, error:menu.model.error,
        busy:menu.active?.model.busy ?? false, status:menu.active?.model.status ?? null};
}
"""


def check(session, output, results):
    session.call("open_scene_preview", {"path":"scenes/terrain-world.scene"})
    session.call("refresh_scene_preview", {"discardChanges":True})
    session.call("close_scene_preview")
    run = lifecycle(session, "start_project", "running")["runId"]
    read = lambda: execute(session, run, READ, None, "mcp-terrain-world.ts")
    click(session, "scene-terrain-world")
    wait_for("world scene", read, lambda v: v.get("selected") == "terrain-world", session.timeout)
    scene = session.call("get_scene", {"limit": 20})["scene"]
    terrain = next(n for n in scene["nodes"] if n["name"] == "World terrain")
    environment = next(n for n in scene["nodes"] if n["name"] == "World environment")
    owner = {"sceneId": scene["sceneId"], "nodeId": terrain["nodeId"]}
    session.call("set_render_feature", {"feature": "rvt_viewer", "enabled": True})
    relative = output.relative_to(PROJECT / "cache")
    def capture(name):
        value = session.capture(str(relative / f"{name}.png"), inspect_black=True)
        require(not value["opaqueBlack"] and value["frame"]["meshDraws"] > 0, "Missing actual terrain rendering")
        results.setdefault("captures", []).append(value)
        return value
    for index, name in enumerate(("Southwest", "Southeast", "Northeast", "Northwest")):
        click(session, f"world-{index}")
        state = wait_for(name, read, lambda v: not v.get("busy") and
            str(v.get("status", "")).startswith(name), session.timeout)
        require(not state["error"], f"World gameplay failed: {state}")
        rvt = wait_for("RVT page preparation", lambda: session.call("get_rvt_status"),
            lambda v: v["effective"] and v["requiredPages"] > 0 and v["readyPages"] == v["requiredPages"], session.timeout)
        results.setdefault("corners", []).append({"name": name, "rvt": rvt})
        capture(name.lower())
    def operation(method, request):
        identity = session.call(f"terrain_request_{method}", {**owner, "request": request})
        identity = {key: identity[key] for key in ("world_epoch", "node_id", "component_instance", "sequence")}
        result = wait_for(f"terrain {method}", lambda: session.call("terrain_operation_status", {**owner, "request": identity}),
            lambda v: v["state"] in ("completed", "failed", "cancelled"), session.timeout)
        require(result["state"] == "completed", f"Terrain {method} failed: {result}")
        session.call("terrain_release_operation", {**owner, "request": identity})
        results.setdefault("operations", []).append(result)
        return result
    edit = operation("edit", {"brushes":[{"center":{"x":-3600,"y":0,"z":3600},
        "extent":{"x":64,"y":64,"z":64},"operation":"dig","shape":"sphere"}]})
    require(int(edit["published_revision"]) > 1, "Excavation did not publish new geometry")
    capture("excavated")
    operation("save", {"slot":"showcase-world-review", "checkpoint_id":output.name})
    bounds = {"minimum":{"x":-3700,"y":-128,"z":3500},"maximum":{"x":-3500,"y":128,"z":3700}}
    operation("reset", {"bounds":bounds})
    capture("reset")
    operation("load", {"slot":"showcase-world-review", "checkpoint_id":output.name})
    capture("restored")
    session.call("set_rvt_debug_options", {"mode":"physical_atlas","channel":"normal","mip":-1})
    capture("normal-atlas")
    settings = session.call("get_render_settings")["rvt"]
    settings["enabled"] = False
    session.call("set_render_settings", {"sceneId":scene["sceneId"],"nodeId":environment["nodeId"],"rvt":settings})
    wait_for("RVT released", lambda: session.call("get_rvt_status"),
        lambda v: not v["requested"] and not v["effective"] and v["residentBytes"] == "0", session.timeout)
    capture("rvt-disabled")
    session.call("set_render_feature", {"feature":"rvt_viewer","enabled":False})
    capture("viewer-disabled")
    click(session, "back-menu")
    wait_for("world unloaded", read, lambda v: not v.get("selected"), session.timeout)
    results["passed"] = True


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--server", required=True, type=Path)
    parser.add_argument("--runtime-dir", required=True, type=Path)
    parser.add_argument("--timeout", type=float, default=180)
    args = parser.parse_args()
    label = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ") + "-" + uuid.uuid4().hex[:8]
    output = PROJECT / "cache/terrain-world" / label
    output.mkdir(parents=True, exist_ok=False)
    results = {"passed":False}
    try:
        with EditorMcpSession(args.server, PROJECT, args.runtime_dir, output / "receipts.jsonl", timeout=args.timeout) as session:
            check(session, output, results)
    finally:
        (output / "result.json").write_text(json.dumps(results, indent=2) + "\n")
        print(f"Terrain world archive: {output}", flush=True)


if __name__ == "__main__":
    main()
