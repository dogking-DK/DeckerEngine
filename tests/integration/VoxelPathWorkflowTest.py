"""M23.12: native voxel paths through real GUI/Console/IPC and frozen captures."""
import argparse
import copy
import json
import math
import shutil
import subprocess
import sys
import time
from pathlib import Path
from uuid import uuid4
from decker import Client, RpcError, guard


def main():
    parser = argparse.ArgumentParser()
    for key in ("editor", "runner", "client", "fixture", "example", "mesh-fixture", "work-dir"):
        parser.add_argument("--" + key, type=Path, required=True)
    parser.add_argument("--no-rt", action="store_true")
    args = parser.parse_args()
    directory = args.work_dir / ("voxel-path-" + uuid4().hex)
    root = directory / "project 稀疏"
    root.mkdir(parents=True)
    subprocess.run([str(args.fixture), "write", str(root)], check=True, timeout=60)
    scene = json.loads((root / "scene.json").read_text())
    scene["entities"][0]["components"]["dk.Transform"]["translation"] = [.5, -.5, 0]
    voxel_entity = scene["entities"][0]["components"]["dk.Identity"]["id"]
    mesh_id, mesh_entity = str(uuid4()), str(uuid4())
    mesh = json.loads((args.mesh_fixture / "back.gltf").read_text())
    mesh["materials"][0]["doubleSided"] = True
    mesh["materials"][0]["pbrMetallicRoughness"]["metallicFactor"] = 0
    (root / "assets/back.gltf").write_text(json.dumps(mesh))
    shutil.copyfile(args.mesh_fixture / "quad.bin", root / "assets/quad.bin")
    project = json.loads((root / "project.json").read_text())
    project["assets"].append(dict(id=mesh_id, kind="mesh", path="assets/back.gltf"))
    entity = copy.deepcopy(scene["entities"][0])
    entity["components"]["dk.Identity"]["id"] = mesh_entity
    entity["components"]["dk.Transform"]["translation"] = [0, 0, 2]
    entity["components"]["dk.AssetReferences"]["items"] = [dict(id=mesh_id, kind="mesh")]
    scene["entities"].append(entity)
    (root / "project.json").write_text(json.dumps(project))
    (root / "scene.json").write_text(json.dumps(scene))
    (root / ".dk-editor-smoke").touch()
    evidence = root / ".dk-voxel-path"
    evidence.mkdir()
    endpoint = "dk-voxel-path-" + uuid4().hex
    report = dict(status="failed", rt_disabled=args.no_rt, checkpoints=[], captures=[])
    process = None
    with (directory / "stdout.log").open("wb") as out, (directory / "stderr.log").open("wb") as err:
        try:
            command = [args.editor, "--background", "--root", root, "--pipe", endpoint, "--fixture-camera", "--validation",
                       "--voxel-path-no-rt" if args.no_rt else "--voxel-path-smoke"]
            process = subprocess.Popen([str(x) for x in command], stdin=subprocess.DEVNULL, stdout=out, stderr=err, creationflags=subprocess.CREATE_NO_WINDOW)
            client = Client(endpoint, args.client, timeout=60)
            def call(method, params=None):
                return client.call(method, params).value
            def checkpoint(name, **fields):
                temp = evidence / "request.tmp"
                temp.write_text(json.dumps(dict(name=name, **fields)))
                deadline = time.monotonic() + 5
                while True:
                    try:
                        temp.replace(evidence / "request.json")
                        break
                    except PermissionError:
                        if time.monotonic() > deadline: raise
                        time.sleep(.01)
                deadline = time.monotonic() + 150
                while not (evidence / (name + ".json")).exists():
                    if process.poll() is not None or time.monotonic() > deadline:
                        raise RuntimeError("Missing " + name + ": " + (directory / "stderr.log").read_text(errors="replace"))
                    time.sleep(.03)
                value = json.loads((evidence / (name + ".json")).read_text())
                if fields.get("expect", "complete") == "complete":
                    q = call("editor.viewport.query")
                    assert q["guard"] == value["guard"] and q["settings"] == value["settings"]
                    assert value["presentation"]["current"] and value["voxel_count"] == 1
                report["checkpoints"].append(value)
                return value
            def change(**fields):
                return call("editor.viewport.set", dict(guard=call("editor.viewport.query")["guard"], **fields))
            def camera(**fields):
                return call("editor.camera.set", dict(guard=call("editor.camera.query")["guard"], **fields))
            def reload():
                return call("scene.load", dict(manifest="project.json", guard=guard(call("scene.query")["state"])))
            def reject(method, params):
                try:
                    call(method, params)
                except RpcError:
                    return
                raise AssertionError("Rejected operation accepted: " + method)
            def finish(ticket, expected="succeeded"):
                deadline = time.monotonic() + 180
                while True:
                    job = call("jobs.wait", dict(id=ticket["job_id"], timeout_ms=100))["job"]
                    if job["state"] in ("succeeded", "failed", "cancelled"):
                        assert job["state"] == expected, job
                        if expected == "succeeded":
                            result = job["result"]
                            assert result["voxel_count"] == 1
                            for field in ("document_id", "scene_id", "revision", "frame", "width", "height"):
                                assert result[field] == ticket[field]
                            report["captures"].append(result)
                        return job
                    assert time.monotonic() < deadline
            def capture(name, reference, **fields):
                params = dict(guard=guard(call("scene.query")["state"]), output=name, width=reference["presentation"]["width"],
                              height=reference["presentation"]["height"], camera=reference["camera"], validation="required")
                if "pathtrace" in reference:
                    params.update(renderer="pathtrace", format="linear_json", pathtrace=reference["pathtrace"])
                params.update(fields)
                return call("render.capture", params)
            def compare(reference, filename=None):
                filename = filename or reference["checkpoint"] + "-capture.json"
                ticket = capture(filename, reference)
                assert ticket["provenance"]["asset_snapshot"] == "accepted" and ticket["provenance"]["asset_cpu_bytes"] > 0
                finish(ticket)
                result = json.loads((root / filename).read_text())
                expected = json.loads((evidence / (reference["checkpoint"] + ".linear.json")).read_text())
                assert result["voxel_count"] == 1 and result["sample_first"] == 0
                assert result["sample_count"] == reference["pathtrace"]["target_spp"]
                assert result["provenance"]["quality"] == "pathtrace_v3"
                assert result["provenance"]["input_fingerprint"] == reference["input_fingerprint"]
                assert result["provenance"]["geometry_fingerprint"] == reference["geometry_fingerprint"]
                error = max(abs(a-b) for row, other in zip(result["linear_rgb"], expected, strict=True) for a, b in zip(row, other, strict=True))
                scale = max(1, max(abs(v) for row in expected for v in row))
                assert error <= 2e-5 * scale, (reference["checkpoint"], error, scale)
                assert all(math.isfinite(v) and v >= 0 for row in result["linear_rgb"] for v in row)
                return result
            base = checkpoint("raster")
            assert base["center_pick"] == voxel_entity and not base["published_path"]
            descriptions = {name: call("commands.describe", dict(name=name)) for name in ("editor.viewport.set", "editor.camera.set", "render.capture")}
            (directory / "descriptions.json").write_text(json.dumps(descriptions, indent=2))
            if args.no_rt:
                q = call("editor.viewport.query")
                assert not q["capabilities"]["pathtracing"] and q["capabilities"]["unavailable"]
                reject("editor.viewport.set", dict(guard=q["guard"], renderer="pathtrace"))
                assert call("editor.viewport.query")["guard"] == q["guard"]
                finish(capture("no-rt-raster.ppm", base))
                call("runtime.shutdown"); assert process.wait(timeout=30) == 0
                assert "validation errors=0 warnings=0 liveAllocations=0" in (directory / "stdout.log").read_text(errors="replace")
                report["status"] = "passed"
                print("Voxel path disabled-RT acceptance passed:", directory)
                return

            initial_scene = call("scene.query")["state"]
            change(target_spp=3, bounces=2, seed=7, environment_rgb=[.3, .4, .6], sky_enabled=True, sun=False)
            gui = checkpoint("gui_path", action="gui_path")
            assert gui["published_path"] and gui["center_pick"] == voxel_entity and gui["bounds"] == base["bounds"]
            compare(gui)
            joint = checkpoint("console_joint", action="console_joint")
            assert joint["settings"]["temporal_reuse"] and joint["settings"]["spatial_reuse"] and joint["console_history"] > 0 and joint["console_empty"]
            assert joint["temporal_reused"] > 0 and joint["spatial_reused"] > 0
            compare(joint)
            for mode, temporal, spatial in (("initial", False, False), ("temporal", True, False), ("spatial", False, True)):
                change(temporal_reuse=temporal, spatial_reuse=spatial)
                current = checkpoint("surface_" + mode)
                compare(current)
            assert call("scene.query")["state"] == initial_scene
            selected = checkpoint("selected", action="select")
            assert selected["selection"] == voxel_entity
            moved = checkpoint("moved", action="gizmo")
            assert moved["scene_revision"] > selected["scene_revision"] and moved["undo"] == selected["undo"] + 1
            assert moved["geometry_fingerprint"] != selected["geometry_fingerprint"] and moved["epoch_first"] == 0
            undone = checkpoint("undone", action="undo")
            assert undone["geometry_fingerprint"] == selected["geometry_fingerprint"] and undone["redo"] == 1
            framed = checkpoint("framed", action="frame")
            assert framed["scene_revision"] == undone["scene_revision"]
            call("editor.camera.reset", dict(guard=call("editor.camera.query")["guard"], defaults=True))
            camera(projection="orthographic", ortho_height=3)
            change(direct_lighting="classic", temporal_reuse=False, spatial_reuse=False)
            ortho = checkpoint("surface_ortho")
            compare(ortho)

            # Native path capture owns accepted bytes even if both voxel and mesh sources disappear.
            original = (root / "assets/solid.dkvoxel").read_bytes()
            mesh_bytes = (root / "assets/back.gltf").read_bytes()
            ticket = capture("frozen.json", ortho)
            (root / "assets/solid.dkvoxel").write_text("broken after acceptance")
            (root / "assets/back.gltf").write_text("broken mesh after acceptance")
            finish(ticket)
            assert json.loads((root / "frozen.json").read_text())["linear_rgb"] == json.loads((root / "surface_ortho-capture.json").read_text())["linear_rgb"]
            reject("render.capture", dict(guard=guard(call("scene.query")["state"]), renderer="pathtrace", output="frozen.json", format="linear_json", pathtrace=ortho["pathtrace"], camera=ortho["camera"]))
            (root / "assets/back.gltf").write_bytes(mesh_bytes)
            reload()
            failed = checkpoint("failed_reload", expect="failed")
            assert not failed["presentation"]["current"] and failed["pixels"] == ortho["pixels"]
            (root / "assets/solid.dkvoxel").write_bytes(original)
            reload(); recovered = checkpoint("recovered")
            assert recovered["epoch_first"] == 0 and recovered["history_empty"]
            palette = json.loads(original); palette["palette"][0]["base_color"] = [.8, .1, .2]
            (root / "assets/solid.dkvoxel").write_text(json.dumps(palette)); reload()
            edited = checkpoint("palette")
            assert edited["geometry_fingerprint"] != recovered["geometry_fingerprint"] and edited["epoch_first"] == 0
            assert edited["pixels"] != recovered["pixels"]
            palette["grid"] = "alternate"
            (root / "assets/solid.dkvoxel").write_text(json.dumps(palette)); reload()
            replaced = checkpoint("grid")
            assert replaced["geometry_fingerprint"] != edited["geometry_fingerprint"] and replaced["epoch_first"] == 0
            (root / "assets/solid.dkvoxel").write_bytes(original); reload()
            current = checkpoint("restored")

            query = call("environment.query"); env = query["environment"]
            env["enabled"] = True; env["atmosphere"]["planet_center_m"][1] -= 2000
            env["clouds"].update(enabled=True, coverage=1, weather_resolution=16, base_resolution=16, detail_resolution=16)
            call("environment.set", dict(guard=guard(query["state"]), environment=env))
            change(medium_mode="scene_media", time_seconds=17)
            for mode, temporal, spatial in (("classic", False, False), ("initial", False, False), ("temporal", True, False), ("spatial", False, True), ("joint", True, True)):
                change(direct_lighting="classic" if mode == "classic" else "restir_di", temporal_reuse=temporal, spatial_reuse=spatial)
                current = checkpoint("media_" + mode)
                assert current["epoch_first"] == 0 and current["history_empty"]
                compare(current)
            change(time_seconds=19); wind = checkpoint("wind")
            assert wind["input_fingerprint"] != current["input_fingerprint"] or wind["generation"] != current["generation"]
            assert wind["epoch_first"] == 0
            change(unlit_inspection=True)
            raster = checkpoint("raster_again", action="gui_raster")
            assert not raster["published_path"] and raster["center_pick"] == voxel_entity
            change(renderer="pathtrace", medium_mode="surface_only", direct_lighting="classic", unlit_inspection=False)
            returned = checkpoint("returned")
            assert returned["epoch_first"] == 0
            compare(returned)
            # Mixed picking follows displayed content when a mesh moves ahead of the voxel.
            call("entity.set_transform", dict(guard=guard(call("scene.query")["state"]), id=mesh_entity,
                transform=dict(translation=[0, 0, -.5], rotation=[0, 0, 0, 1], scale=[1, 1, 1])))
            front = checkpoint("mesh_front")
            assert front["center_pick"] == mesh_entity and front["epoch_first"] == 0
            change(target_spp=4096)
            checkpoint("inflight", expect="pending")
            change(renderer="raster", unlit_inspection=True)
            late = checkpoint("late_raster")
            assert not late["published_path"] and late["center_pick"] == mesh_entity
            change(renderer="pathtrace", target_spp=3, unlit_inspection=False)
            final = checkpoint("final")
            assert final["epoch_first"] == 0
            # Closing with the real worker in flight cannot publish a later frame.
            change(target_spp=4096)
            checkpoint("closing", expect="pending")
            call("runtime.shutdown"); assert process.wait(timeout=40) == 0
            logs = (directory / "stdout.log").read_text(errors="replace")
            assert "validation errors=0 warnings=0 liveAllocations=0" in logs

            # Independent runner: the same frozen source and settings reproduce the viewport.
            endpoint += "-runner"
            process = subprocess.Popen([str(args.runner), "--project-root", str(root), "--pipe", endpoint], stdin=subprocess.DEVNULL,
                stdout=out, stderr=err, creationflags=subprocess.CREATE_NO_WINDOW)
            client = Client(endpoint, args.client, timeout=60)
            call("scene.load", dict(manifest="project.json"))
            assert not any(c["name"].startswith("editor.") for c in call("commands.list"))
            # Scene reload discards unsaved edits; compare to the accepted restored source.
            finish(capture("standalone.json", recovered))
            standalone = json.loads((root / "standalone.json").read_text())
            expected = json.loads((evidence / "recovered.linear.json").read_text())
            assert standalone["linear_rgb"] == expected
            for version in (1, 2):
                p = dict(version=version, target_spp=2, seed=7, max_bounces=2, rr_start=2, environment_rgb=[.3, .4, .6])
                if version == 2: p["medium_mode"] = "surface_only"
                finish(capture("legacy%d.json" % version, recovered, pathtrace=p))
            # Executable example uses the same public command, including ReSTIR and orthographic camera.
            example = subprocess.run([sys.executable, str(args.example), "--endpoint", endpoint, "--client", str(args.client),
                "--pathtrace", "--restir", "joint", "--orthographic", "--width", "16", "--height", "16", "--output", "example.json"],
                capture_output=True, text=True, timeout=150, creationflags=subprocess.CREATE_NO_WINDOW)
            assert example.returncode == 0, (example.stdout, example.stderr)
            assert json.loads((root / "example.json").read_text())["voxel_count"] == 1
            (root / "cancel.json").write_text("old output")
            long_path = dict(recovered["pathtrace"], target_spp=4096)
            long = capture("cancel.json", recovered, width=32, height=32, pathtrace=long_path)
            deadline = time.monotonic() + 120
            while call("jobs.wait", dict(id=long["job_id"], timeout_ms=20))["job"]["progress"]["completed"] < 1:
                assert time.monotonic() < deadline
            assert call("jobs.cancel", dict(id=long["job_id"]))["accepted"]
            finish(long, "cancelled"); assert (root / "cancel.json").read_text() == "old output"
            (root / "close.json").write_text("old output")
            closing = capture("close.json", recovered, width=32, height=32, pathtrace=long_path)
            deadline = time.monotonic() + 120
            while call("jobs.wait", dict(id=closing["job_id"], timeout_ms=20))["job"]["progress"]["completed"] < 1:
                assert time.monotonic() < deadline
            call("runtime.shutdown"); assert process.wait(timeout=40) == 0
            assert (root / "close.json").read_text() == "old output"
            report["status"] = "passed"
        finally:
            if process is not None and process.poll() is None:
                process.kill(); process.wait(timeout=15)
            (directory / "summary.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
    print("Voxel native path consumer acceptance passed:", directory)


if __name__ == "__main__":
    main()
