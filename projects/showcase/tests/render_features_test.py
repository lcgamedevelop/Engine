#!/usr/bin/env python3
"""Validate standalone render cases through actual MCP, RmlUi and TS without performance scores.

Connect to an imported showcase editor without starting or closing it.
Use 384 fixed objects and same-camera submission counts; keep captures and receipts in the project cache.
"""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import json
from pathlib import Path
import sys
import struct
import uuid
import zlib

from mcp_interaction_test import (PROJECT, EditorMcpSession, capture_for_review, click, execute,
                                  get_status, lifecycle, require, wait_for)


READ = """
import RenderFeatures from "./modules/render-features.js";
export default function(_args: unknown, gameplay: unknown) {
    const menu = gameplay as { active: RenderFeatures | null;
        model: { selected: {id: string} | null; error: string | null } };
    return { selected: menu.model.selected?.id ?? null, error: menu.model.error,
        rendering: menu.active instanceof RenderFeatures ? menu.active.Inspect() : null };
}
"""


def expect_failure(session, name, arguments):
    try:
        session.call(name, arguments)
    except RuntimeError:
        return
    raise RuntimeError(f"Invalid request {name} was accepted: {arguments}")


def read_lab(session, run):
    value = execute(session, run, READ, None, "mcp-render-features.ts")
    require(value["error"] is None, f"Render case startup failed: {value}")
    return value


def ready(session, run):
    value = wait_for("standalone render case", lambda: read_lab(session, run),
        lambda item: item["selected"] == "render-features" and item["rendering"] is not None and
                     item["rendering"]["objects"] == 384,
        session.timeout)
    return value["rendering"]


def states(session):
    result = session.call("get_render_features")["renderer"]
    require(result["scope"] == "world_summary", "Render summary must state the actual world submission scope")
    return result["features"]


def wait_feature(session, feature, enabled, after=0):
    def read():
        result = session.call("get_render_features")
        return {"frame": result["renderer"]["frameNumber"],
                "state": result["renderer"]["features"][feature]}
    return wait_for(f"{feature} actual rendering state", read,
        lambda item: item["frame"] > after and item["state"]["requested"] == enabled and
                     item["state"]["effective"] == enabled,
        session.timeout)["state"]


def main_stats(session, after=0):
    result = wait_for("new main-view submission", lambda: session.call("get_render_submission_stats"),
        lambda item: item["frameNumber"] > after and
                     any(row["viewId"] == 113 and row["candidateMeshes"] >= 384
                         for row in item["views"]), session.timeout)
    require(result["source"] == "scene_mesh_submission" and
            result["gpuCompletion"] == "not_measured", "Submission counts must not be presented as GPU timing or completion")
    return next(row for row in result["views"] if row["viewId"] == 113)


def selected_ground(session):
    summary = session.call("get_scene", {"limit": 0})
    count = summary["totalNodes"]
    # Ground is appended after the 384 spheres. Read a bounded page, not a >1 MiB scene receipt.
    result = session.call("get_scene", {"offset": max(0, count - 16), "limit": 16})
    nodes = [node for node in result["scene"]["nodes"] if node["name"] == "Hex surface"]
    require(len(nodes) == 1, "Expected one Hex material test floor")
    return {"sceneId": result["scene"]["sceneId"], "nodeId": nodes[0]["nodeId"], "slot": 0}


def wait_scoped(session, scope, feature, enabled, identity=None):
    return wait_for(f"{scope}/{feature} actual value", lambda: session.call("get_render_features", identity or {}),
        lambda item: item[scope]["features"][feature]["requested"] == enabled and
                     (scope == "material" or item[scope]["features"][feature]["effective"] == enabled),
        session.timeout)[scope]


def check(session, results, output):
    status = get_status(session)
    require(status.get("editorAlive") is True, "Start the showcase editor and finish asset import first")
    if not status.get("running"):
        status = lifecycle(session, "start_project", "running")
    run = status["runId"]
    require(read_lab(session, run)["selected"] is None, "Return to the showcase menu before render checks")
    previous = {name: state["requested"] for name, state in states(session).items()}
    click(session, "scene-render")
    results["initial"] = ready(session, run)
    results["baseline"] = main_stats(session)
    require(results["baseline"]["culledMeshes"] > 0 and
            results["baseline"]["directInstanceBatches"] > 0,
            "Fixed scene does not actually use frustum culling and static instancing")
    capture_for_review(session, results, output, "initial")
    lod = session.call("get_mesh_lod_import", {"path": "meshes/render-lod-sphere.mesh"})
    require(lod["settings"]["enabled"] and len(lod["meshes"]) == 1, "LOD does not use imported resources")
    levels = lod["meshes"][0]["levels"]
    require(len(levels) >= 2 and all(levels[index]["triangleCount"] > levels[index + 1]["triangleCount"]
                                     for index in range(len(levels) - 1)), f"Imported LOD did not reduce geometry: {lod}")
    results["importedLods"] = lod

    for feature in ("frustum_culling", "static_instancing", "static_lod"):
        before = main_stats(session)
        click(session, f"render-{feature}")
        wait_feature(session, feature, False, before["frameNumber"])
        disabled = main_stats(session, before["frameNumber"])
        require(ready(session, run)["objects"] == 384, f"{feature} changed the scene object count")
        if feature == "frustum_culling":
            require(disabled["culledMeshes"] == 0 and disabled["visibleMeshes"] >= before["visibleMeshes"],
                    "Objects were still culled after culling was disabled")
        elif feature == "static_instancing":
            require(disabled["directInstanceBatches"] == 0 and disabled["drawCalls"] > before["drawCalls"],
                    "Disabling instancing did not restore individual draw submissions")
        else:
            require(disabled["directTriangles"] > before["directTriangles"], "Disabling LOD did not restore LOD0 topology")
        click(session, f"render-{feature}")
        wait_feature(session, feature, True, disabled["frameNumber"])
        results.setdefault("pathComparisons", {})[feature] = {"enabled": before, "disabled": disabled}

    # Change camera through the normal View/Controller route, then compare LOD on/off at that same camera.
    click(session, "render-camera")
    wait_for("distant viewing camera", lambda: ready(session, run), lambda item: item["distant"], session.timeout)
    far_on = main_stats(session)
    click(session, "render-static_lod")
    wait_feature(session, "static_lod", False, far_on["frameNumber"])
    far_off = main_stats(session, far_on["frameNumber"])
    require(far_off["directTriangles"] > far_on["directTriangles"], "Toggling LOD from the distant view did not change the triangle count")
    click(session, "render-static_lod")
    wait_feature(session, "static_lod", True, far_off["frameNumber"])
    click(session, "render-camera")
    wait_for("restore original camera", lambda: ready(session, run), lambda item: not item["distant"], session.timeout)
    results["farCameraLod"] = {"enabled": far_on, "disabled": far_off}

    for feature in ("wireframe", "mesh_bounds", "camera_frusta", "stats"):
        original = states(session)[feature]
        require(original["supported"], f"Current device does not support {feature}; this check cannot be reported as complete")
        session.call("set_render_feature", {"feature": feature, "enabled": not original["requested"]})
        changed = wait_feature(session, feature, not original["requested"])
        if feature == "wireframe":
            capture_for_review(session, results, output, "wireframe")
        session.call("set_render_feature", {"feature": feature, "enabled": original["requested"]})
        wait_feature(session, feature, original["requested"])
        results.setdefault("mcpDebug", {})[feature] = changed

    session.call("set_render_feature", {"feature": "debug_draw", "enabled": False})
    wait_feature(session, "debug_draw", False)
    line = {"start": [0, 1, 0], "end": [0, 5, 0], "color": 4294902015, "depthTest": True}
    expect_failure(session, "draw_debug_lines", {"lines": [line]})
    session.call("set_render_feature", {"feature": "debug_draw", "enabled": True})
    require(states(session)["debug_draw"]["requested"], "Debug drawing request was not accepted")
    results["debugLines"] = session.call("draw_debug_lines", {"lines": [line], "lifetimeFrames": 600})
    require(results["debugLines"]["acceptedLines"] == 1, "Debug lines were not actually accepted")
    wait_feature(session, "debug_draw", True)
    capture_for_review(session, results, output, "debug-lines")

    click(session, "render-atmosphere")
    wait_for("UI sky disabled", lambda: ready(session, run), lambda item: not item["atmosphere"], session.timeout)
    wait_scoped(session, "scene", "atmosphere", False)
    capture_for_review(session, results, output, "sky-off")
    click(session, "render-atmosphere")
    environment = wait_scoped(session, "scene", "atmosphere", True)
    env_identity = {key: environment[key] for key in ("sceneId", "nodeId")}
    for enabled in (False, True):
        session.call("set_render_feature", {"feature": "atmosphere", "enabled": enabled, **env_identity})
        wait_scoped(session, "scene", "atmosphere", enabled)

    ground = selected_ground(session)
    click(session, "render-hex")
    wait_for("UI Hex enabled", lambda: ready(session, run), lambda item: item["hex"], session.timeout)
    wait_scoped(session, "material", "hex_tiling", True, ground)
    capture_for_review(session, results, output, "hex-on")
    click(session, "render-hex")
    wait_scoped(session, "material", "hex_tiling", False, ground)
    for enabled in (True, False):
        session.call("set_render_feature", {"feature": "hex_tiling", "enabled": enabled, **ground})
        wait_scoped(session, "material", "hex_tiling", enabled, ground)

    # A per-object edit cannot turn off other objects; native readback remains separate from scene summaries.
    original_ground = session.call("get_mesh_render_settings", {key: ground[key] for key in ("sceneId", "nodeId")})
    for feature, field in (("static_instancing", "staticInstancingEnabled"), ("static_lod", "staticLodEnabled")):
        identity = {key: ground[key] for key in ("sceneId", "nodeId")}
        session.call("set_render_feature", {"feature": feature, "enabled": False, **identity})
        require(session.call("get_mesh_render_settings", identity)[field] is False,
                f"{feature} did not update the selected MeshRenderer")
        require(states(session)[feature]["requested"], "A per-object property incorrectly changed global policy for other objects")
        session.call("undo", {"sceneId": ground["sceneId"]})
        require(session.call("get_mesh_render_settings", identity)[field] == original_ground[field], "Undo did not restore component properties")
        session.call("redo", {"sceneId": ground["sceneId"]})
        require(session.call("get_mesh_render_settings", identity)[field] is False, "Redo did not restore this object's property change")
        session.call("undo", {"sceneId": ground["sceneId"]})
    camera = session.call("get_render_features")["camera"]
    camera_identity = {key: camera[key] for key in ("sceneId", "nodeId")}
    session.call("set_render_feature", {"feature": "frustum_culling", "enabled": False, **camera_identity})
    require(session.call("get_render_features", camera_identity)["camera"]["frustumCullingEnabled"] is False,
            "Unified culling entry did not edit Camera properties")
    session.call("undo", {"sceneId": camera["sceneId"]})
    require(session.call("get_render_features", camera_identity)["camera"]["frustumCullingEnabled"] is True,
            "Camera property undo failed")
    sun = session.call("get_render_settings")["atmosphereSunNodeId"]
    require(isinstance(sun, str) and sun != "0", "Environment query did not return the actual sun Node identity")
    session.call("set_render_settings", {**env_identity, "atmosphereSunNodeId": sun})
    require(session.call("get_render_settings")["atmosphereSunNodeId"] == sun, "Incorrect sun Node/local ID conversion")
    expect_failure(session, "set_render_settings", {**env_identity, "atmosphereSunNodeId": ground["nodeId"]})
    for feature in ("frustum_culling", "static_instancing", "static_lod"):
        expect_failure(session, "set_render_feature", {"feature": feature, "enabled": False})

    session.call("set_render_feature", {"feature": "mesh_bounds", "enabled": True})
    require(wait_feature(session, "mesh_bounds", True)["requested"], "Unified renderer toggle was not applied")
    session.call("set_render_feature", {"feature": "mesh_bounds", "enabled": False})
    wait_feature(session, "mesh_bounds", False)
    expect_failure(session, "set_render_feature", {"feature": "frame_view_uniforms", "enabled": False})
    expect_failure(session, "set_render_feature", {"feature": "hex_tiling", "enabled": True, **ground, "slot": 65535})
    expect_failure(session, "set_render_feature", {"feature": "atmosphere", "enabled": False,
                   "sceneId": ground["sceneId"], "nodeId": ground["nodeId"]})
    require(wait_scoped(session, "scene", "atmosphere", True), "Invalid target changed the actual environment")

    for feature in ("wireframe", "debug_draw", "mesh_bounds", "camera_frusta", "stats"):
        session.call("set_render_feature", {"feature": feature, "enabled": previous[feature]})
    click(session, "back-menu")
    results["returned"] = wait_for("return-to-menu cleanup", lambda: read_lab(session, run),
        lambda item: item["selected"] is None and item["rendering"] is None, session.timeout)
    require({name: state["requested"] for name, state in states(session).items()} == previous,
            "Returning to the menu did not restore earlier render requests")
    remaining = session.call("get_scene", {"limit": 0})
    require(remaining["scene"] is None and remaining["totalNodes"] == 0, "Render scene objects remain after returning to the menu")
    # Reopen checks that scene material/environment changes and temporary debug work were not saved.
    click(session, "scene-render")
    reopened = ready(session, run)
    require(reopened["objects"] == 384 and reopened["atmosphere"] and not reopened["hex"] and
            not reopened["distant"], "Reentry inherited temporary edits from the previous run")
    results["reopened"] = reopened
    click(session, "back-menu")
    wait_for("return to menu again", lambda: read_lab(session, run),
        lambda item: item["selected"] is None and item["rendering"] is None, session.timeout)
    print("PASS 384 fixed objects: actual UI/MCP toggles, imported LOD, per-view submissions, debug drawing, sky/Hex and menu cleanup", flush=True)


def meshlet_check(session, results, output):
    read_source = READ.replace("RenderFeatures", "MeshletCulling").replace(
        "./modules/render-features.js", "./modules/meshlet-culling.js")
    status = get_status(session)
    if not status.get("running"):
        status = lifecycle(session, "start_project", "running")
    run = status["runId"]

    def read():
        value = execute(session, run, read_source, None, "mcp-meshlet.ts")
        require(value["error"] is None, f"Meshlet scene failed: {value}")
        return value

    def stats(after=0):
        value = wait_for("meshlet main frame", lambda: session.call("get_render_submission_stats"),
            lambda item: item["frameNumber"] > after and any(
                row["viewId"] == 113 and row["candidateMeshes"] == 28 for row in item["views"]),
            session.timeout)
        return next(row for row in value["views"] if row["viewId"] == 113)

    def frame_pixels(path):
        # Runtime captures have a fixed unfiltered RGBA PNG contract, also used by
        # EditorMcpSession. Compare below the UI without requiring image packages.
        data = Path(path).read_bytes()
        require(data[:8] == b"\x89PNG\r\n\x1a\n", "Expected PNG capture")
        offset, compressed, header = 8, bytearray(), None
        while offset + 12 <= len(data):
            length = struct.unpack_from(">I", data, offset)[0]
            kind = data[offset + 4:offset + 8]
            payload = data[offset + 8:offset + 8 + length]
            if kind == b"IHDR":
                header = struct.unpack(">IIBBBBB", payload)
            elif kind == b"IDAT":
                compressed.extend(payload)
            offset += length + 12
        require(header and header[2:] == (8, 6, 0, 0, 0), "Unexpected capture pixel format")
        width, height = header[:2]
        raw = zlib.decompress(compressed)
        stride = width * 4 + 1
        require(len(raw) == stride * height and all(raw[y * stride] == 0 for y in range(height)),
                "Unexpected PNG row filtering")
        # Exclude the controls in the top quarter; compare the rendered scene.
        return header[:2], b"".join(raw[y * stride + 1:(y + 1) * stride]
                                   for y in range(height // 4, height))

    def compare(a, b, label):
        size_a, left = frame_pixels(a["path"])
        size_b, right = frame_pixels(b["path"])
        require(size_a == size_b, "Comparison requires identical dimensions")
        maximum = max(abs(x - y) for x, y in zip(left, right))
        changed = sum(abs(x - y) > 2 for x, y in zip(left, right))
        results.setdefault("pixelComparisons", {})[label] = {
            "maximumChannelDifference": maximum, "channelsOverTwo": changed,
            "comparedChannels": len(left)}
        require(changed == 0,
                f"Meshlet {label} changed scene pixels: {changed}, max {maximum}")

    require(read()["selected"] is None, "Return to the menu before Meshlet checks")
    click(session, "scene-meshlet")
    wait_for("meshlet scene", read, lambda item: item["selected"] == "meshlet-culling" and
             item["rendering"] is not None and item["rendering"]["objects"] == 29, session.timeout)
    require(not read()["rendering"]["enabled"], "Scene meshlets must default off")
    baseline = stats()
    require(baseline["indirectBatches"] == 0, "Disabled scene submitted meshlets")
    ordinary = capture_for_review(session, results, output, "ordinary")
    imported = session.call("get_meshlet_import", {"path": "meshes/meshlet-lod-sphere.mesh"})
    levels = imported["meshes"][0]["levels"]
    require(imported["settings"]["enabled"] and len(levels) > 1 and
            all(level["meshletCount"] > 0 for level in levels), "LOD fixture lacks cluster data")
    results["imported"] = imported
    expect_failure(session, "set_render_feature", {"feature": "meshlet_culling", "enabled": True})
    click(session, "meshlet-toggle")
    wait_feature(session, "meshlet_culling", True, baseline["frameNumber"])
    active = stats(baseline["frameNumber"])
    require(active["indirectBatches"] > 0 and active["meshletInstancedBatches"] > 0 and
            active["meshletMissingData"] > 0 and active["meshletIncompatibleDraws"] > 0,
            f"Expected shared indirect batches and explicit ordinary fallbacks: {active}")
    require(states(session)["static_instancing"]["effective"],
            "Meshlet instance batches must count as effective static instancing")
    require(read()["rendering"]["enabled"], "TS did not expose the real scene switch")
    transparent = next(row for row in session.call("get_render_submission_stats")["views"]
                       if row["viewId"] == 130)
    require(transparent["directInstances"] == 1 and transparent["indirectBatches"] == 0,
            "Transparent control must keep its ordinary sorted pass")
    optimized = capture_for_review(session, results, output, "meshlets")
    compare(ordinary, optimized, "perspective")
    results.update(baseline=baseline, active=active)
    session.call("set_render_feature", {"feature": "meshlet_statistics", "enabled": True})
    sampled = wait_for("completed meshlet GPU sample", stats,
        lambda row: row["meshletGpu"]["state"] in ("ready", "failed"), session.timeout)
    gpu = sampled["meshletGpu"]
    require(gpu["state"] == "ready" and int(gpu["worldEpoch"]) > 0 and
            int(gpu["observer"][1]) > 0 and gpu["frameNumber"] > active["frameNumber"] and
            0 < gpu["visiblePairs"] < gpu["candidatePairs"] and
            0 < gpu["visibleTriangles"] < gpu["candidateTriangles"], f"Invalid GPU sample: {gpu}")
    results["gpu"] = gpu
    identity = {key: session.call("get_render_features")["scene"][key] for key in ("sceneId", "nodeId")}

    # Exercise the same scene property through the editor and the project UI.
    session.call("set_render_feature", {"feature": "meshlet_culling", "enabled": False, **identity})
    wait_feature(session, "meshlet_culling", False)
    require(not read()["rendering"]["enabled"] and stats()["indirectBatches"] == 0,
            "Editor switch did not restore the ordinary path")
    session.call("set_render_feature", {"feature": "meshlet_colors", "enabled": True})
    wait_feature(session, "meshlet_colors", True)
    colored = capture_for_review(session, results, output, "cluster-colors")
    require(not read()["rendering"]["enabled"] and stats()["indirectBatches"] == 0,
            "Cluster colors changed the scene culling switch")
    _, normal_pixels = frame_pixels(ordinary["path"])
    _, color_pixels = frame_pixels(colored["path"])
    require(sum(abs(a - b) > 8 for a, b in zip(normal_pixels, color_pixels)) > 1000,
            "Cluster colors did not paint the actual mesh surfaces")
    repeated = capture_for_review(session, results, output, "cluster-colors-repeat")
    compare(colored, repeated, "stable-cluster-colors")
    instance_source = """
import { MeshRenderer } from "engine";
import MeshletCulling from "./modules/meshlet-culling.js";
export default function(enabled: boolean, gameplay: unknown) {
    const scene = (gameplay as {active: MeshletCulling}).active.scene;
    const names = [];
    for (let row = 0; row < 3; ++row) for (let column = 0; column < 6; ++column)
        names.push(`Cluster sphere ${row}:${column}`);
    for (const x of [-9, -3, 3, 9]) names.push(`Arch ${x}`);
    return names.every(name => scene?.FindNode(name)?.GetComponent(MeshRenderer)?.SetStaticInstancing(enabled));
}
"""
    require(execute(session, run, instance_source, False, "mcp-meshlet-instances.ts"),
            "Cannot switch the shared geometry to direct draws")
    direct_colors = capture_for_review(session, results, output, "cluster-colors-direct")
    (width, _), direct_pixels = frame_pixels(direct_colors["path"])
    # Compare solid interiors against direct rendering. Exclude only a one-pixel
    # band around reference color boundaries, where raster coverage can differ.
    # Speckles in the tested instanced image cannot exclude their own pixels.
    height = len(color_pixels) // (width * 4)
    edge_pixels = 0
    for offset in range(0, len(color_pixels), 4):
        if max(abs(color_pixels[offset + c] - direct_pixels[offset + c]) for c in range(3)) <= 2:
            continue
        y, x = divmod(offset // 4, width)
        neighbors = [(yy * width + xx) * 4
                     for yy in range(max(0, y - 1), min(height, y + 2))
                     for xx in range(max(0, x - 1), min(width, x + 2))]
        boundary = any(max(direct_pixels[p + c] for p in neighbors) -
                       min(direct_pixels[p + c] for p in neighbors) > 4 for c in range(3))
        require(boundary, f"Instanced cluster color has an uncovered surface pixel at {x},{y}")
        edge_pixels += 1
    results["clusterCoveragePassed"] = True
    results["clusterCoverageEdgePixels"] = edge_pixels
    require(execute(session, run, instance_source, True, "mcp-meshlet-instances.ts"),
            "Cannot restore shared geometry instancing")
    session.call("set_render_feature", {"feature": "meshlet_bounds", "enabled": True})
    wait_feature(session, "meshlet_bounds", True)
    capture_for_review(session, results, output, "cluster-colors-and-bounds")
    session.call("set_render_feature", {"feature": "meshlet_colors", "enabled": False})
    wait_feature(session, "meshlet_colors", False)
    session.call("set_render_feature", {"feature": "meshlet_bounds", "enabled": False})
    wait_feature(session, "meshlet_bounds", False)
    restored = capture_for_review(session, results, output, "cluster-colors-restored")
    compare(ordinary, restored, "restored-cluster-colors")
    session.call("set_render_feature", {"feature": "meshlet_bounds", "enabled": True})
    wait_feature(session, "meshlet_bounds", True)
    capture_for_review(session, results, output, "bounds-while-disabled")
    require(not read()["rendering"]["enabled"], "Debug bounds enabled the runtime mode")
    session.call("set_render_feature", {"feature": "meshlet_bounds", "enabled": False})

    # Native regression covers whole-batch budget overflow. This real-project route
    # covers orthographic cone/frustum logic and a second camera distance using imported LODs.
    camera_source = """
import { Camera, Transform } from "engine";
import MeshletCulling from "./modules/meshlet-culling.js";
export default function(_args: unknown, gameplay: unknown) {
    const scene = (gameplay as {active: MeshletCulling}).active.scene;
    const node = scene?.FindNode("Render Camera");
    return !!node?.GetComponent(Camera)?.SetOrthographic(20, 0.1, 400) &&
           !!node.GetComponent(Transform)?.SetPosition(24, 18, 42);
}
"""
    require(execute(session, run, camera_source, None, "mcp-meshlet-camera.ts"), "Cannot change camera")
    ortho_off = capture_for_review(session, results, output, "orthographic-ordinary")
    session.call("set_render_feature", {"feature": "meshlet_culling", "enabled": True, **identity})
    wait_feature(session, "meshlet_culling", True)
    ortho_on = capture_for_review(session, results, output, "orthographic-meshlets")
    compare(ortho_off, ortho_on, "orthographic")
    session.call("set_render_feature", {"feature": "meshlet_statistics", "enabled": False})
    wait_feature(session, "meshlet_statistics", False)
    click(session, "back-menu")
    wait_for("meshlet scene cleanup", read, lambda item: item["selected"] is None, session.timeout)
    click(session, "scene-meshlet")
    wait_for("fresh meshlet scene", read, lambda item: item["rendering"] is not None, session.timeout)
    require(not read()["rendering"]["enabled"] and stats()["indirectBatches"] == 0,
            "Reentry retained a previous scene's runtime switch or indirect work")
    click(session, "back-menu")
    wait_for("final meshlet cleanup", read, lambda item: item["selected"] is None, session.timeout)
    print("PASS Meshlet GPU visibility, scene toggle, LOD/material fallbacks, debug isolation, pixels and cleanup", flush=True)


def vsm_check(session, results, output):
    status = get_status(session)
    require(status.get("editorAlive"), "Start the imported showcase editor first")
    if not status.get("running"):
        status = lifecycle(session, "start_project", "running")
    run = status["runId"]
    source = """
import { Light, Transform } from "engine";
import VirtualShadows from "./modules/virtual-shadows.js";
export default function(args: { method?: 0 | 1; move?: boolean }, gameplay: unknown) {
    const menu = gameplay as {active: VirtualShadows | null; model: {selected: {id: string} | null; error: string | null}};
    const lab = menu.active instanceof VirtualShadows ? menu.active : null;
    const light = lab?.scene?.FindNode("Sun")?.GetComponent(Light);
    let accepted = true;
    if (args?.method !== undefined) accepted = !!light?.SetShadowMethod(args.method);
    if (args?.move !== undefined) accepted = !!lab?.scene?.FindNode("Moving caster")?.GetComponent(Transform)?.SetPosition(args.move ? 4 : 0, 1.5, 5);
    return {selected: menu.model.selected?.id ?? null, error: menu.model.error, accepted, rendering: lab?.Inspect() ?? null};
}
"""
    def control(arguments=None):
        result = execute(session, run, source, arguments or {}, "mcp-virtual-shadows.ts")
        require(result["error"] is None and result["accepted"], f"VSM scene operation failed: {result}")
        return result

    require(control()["selected"] is None, "Return to the menu before VSM checks")
    click(session, "scene-vsm")
    wait_for("VSM scene", control, lambda row: row["rendering"] is not None, session.timeout)

    def ready(after=0):
        def read():
            row = session.call("get_vsm_status")
            require(row["state"] not in {"failed", "unsupported"}, f"VSM failed: {row}")
            return row
        return wait_for("resident VSM receiver pages", read,
            lambda row: row["state"] == "ready" and row["demandedPages"] > 0 and
                row["fallbackPages"] == 0 and row["redrawnPages"] == 0 and
                int(row["submissionId"]) > after and row["frame"]["ready"], session.timeout)

    results["initial"] = ready()
    capture_for_review(session, results, output, "virtual-shadows")
    results["cached"] = ready(int(results["initial"]["submissionId"]))
    require(results["cached"]["cacheRevision"] == results["initial"]["cacheRevision"],
            "Static VSM pages were unnecessarily invalidated")
    control({"move": True})
    results["moved"] = ready(int(results["cached"]["submissionId"]))
    require(int(results["moved"]["cacheRevision"]) > int(results["cached"]["cacheRevision"]),
            "Moving caster did not invalidate the shadow cache")
    capture_for_review(session, results, output, "moved-caster")
    session.call("set_render_feature", {"feature": "vsm_viewer", "enabled": True})
    for mode in ("overview", "virtual_depth", "physical_atlas", "page_table", "residency", "invalidated"):
        receipt = session.call("set_vsm_debug_options", {"mode": mode, "clipmapLevel": -1})
        wait_for("rendered VSM diagnostics", lambda: session.call("get_vsm_debug_options"),
            lambda row: row["frameDebugVisible"] and row["effective"] and
                row["frameDebugRevision"] == receipt["revision"], session.timeout)
        capture_for_review(session, results, output, mode)
    expect_failure(session, "set_vsm_debug_options", {"mode": "overview", "clipmapLevel": 16})
    expect_failure(session, "set_vsm_debug_options", {"mode": "overview", "clipmapLevel": 15})
    session.call("set_render_feature", {"feature": "vsm_viewer", "enabled": False})
    wait_feature(session, "vsm_viewer", False)
    control({"method": 0})
    results["disabled"] = wait_for("VSM release", lambda: session.call("get_vsm_status"),
        lambda row: row["state"] == "disabled" and row["residentBytes"] == "0", session.timeout)
    capture_for_review(session, results, output, "ordinary-shadows")
    control({"method": 1})
    results["reenabled"] = ready()
    require(results["reenabled"]["generation"] != results["initial"]["generation"],
            "Re-enabling VSM accepted a retired cache generation")
    capture_for_review(session, results, output, "reenabled")
    click(session, "back-menu")
    wait_for("VSM scene cleanup", control, lambda row: row["selected"] is None, session.timeout)
    wait_for("VSM cleanup", lambda: session.call("get_vsm_status"),
        lambda row: row["state"] == "disabled" and row["residentPages"] == 0, session.timeout)
    print("PASS VSM cache reuse, dynamic invalidation, MCP diagnostics, toggle and cleanup", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--server", required=True, type=Path)
    parser.add_argument("--runtime-dir", required=True, type=Path)
    parser.add_argument("--timeout", type=float, default=180)
    parser.add_argument("--case", choices=("rendering", "meshlets", "vsm"), default="rendering")
    args = parser.parse_args()
    if args.timeout <= 0:
        parser.error("--timeout must be positive")
    label = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ") + "-" + uuid.uuid4().hex[:8]
    directory = {"rendering": "render-features", "meshlets": "meshlet-culling", "vsm": "virtual-shadows"}[args.case]
    output = PROJECT / "cache" / "visual-tests" / directory / label
    output.mkdir(parents=True, exist_ok=False)
    results = {"passed": False, "case": args.case,
               "timingsCollected": False}
    try:
        with EditorMcpSession(args.server, PROJECT, args.runtime_dir, output / "receipts.jsonl",
                              timeout=args.timeout) as session:
            {"meshlets": meshlet_check, "vsm": vsm_check, "rendering": check}[args.case](session, results, output)
        results["functionalChecksPassed"] = True
        require(not results.get("invalidCaptures"),
                f"Screenshots are black or could not be inspected: {results.get('invalidCaptures')}")
        results["passed"] = True
        print(f"Receipts, submission counts and captures: {output}")
        return 0
    except (OSError, ValueError, KeyError, RuntimeError) as error:
        results["error"] = str(error)
        print(f"Render functional checks failed: {error}\nPreserved receipts: {output}", file=sys.stderr)
        return 1
    finally:
        (output / "result.json").write_text(json.dumps(results, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    raise SystemExit(main())
