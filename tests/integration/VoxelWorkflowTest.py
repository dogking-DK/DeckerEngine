"""M23.6: real Editor GUI/Console/IPC, entity edits, frozen independent capture."""
import argparse
import json
import copy
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
    args = parser.parse_args()
    directory = args.work_dir / ("voxel-workflow-" + uuid4().hex)
    root = directory / "project 稀疏"
    root.mkdir(parents=True)
    subprocess.run([str(args.fixture), "write", str(root)], check=True, timeout=60)
    scene = json.loads((root / "scene.json").read_text())
    scene["entities"][0]["components"]["dk.Transform"]["translation"] = [.5, -.5, 0]
    (root / "scene.json").write_text(json.dumps(scene), encoding="utf-8")
    (root / ".dk-editor-smoke").touch()
    report = {"status": "failed", "checkpoints": [], "captures": []}
    process = None
    endpoint = "dk-voxel-" + uuid4().hex
    with (directory / "stdout.log").open("wb") as out, (directory / "stderr.log").open("wb") as err:
        try:
            process = subprocess.Popen([str(args.editor), "--background", "--root", str(root), "--pipe", endpoint,
                "--fixture-camera", "--validation", "--voxel-workflow-smoke"], stdin=subprocess.DEVNULL,
                stdout=out, stderr=err, creationflags=subprocess.CREATE_NO_WINDOW)
            client = Client(endpoint, args.client, timeout=30)
            def call(method, params=None):
                return client.call(method, params).value
            evidence = root / ".dk-voxel"
            def checkpoint(name):
                deadline = time.monotonic() + 100
                while not (evidence / (name + ".json")).exists():
                    if process.poll() is not None or time.monotonic() > deadline:
                        raise RuntimeError("Missing " + name + ": " + (directory / "stderr.log").read_text(errors="replace"))
                    time.sleep(.03)
                value = json.loads((evidence / (name + ".json")).read_text())
                q = call("editor.viewport.query")
                assert q["guard"] == value["guard"] and q["settings"] == value["settings"]
                report["checkpoints"].append(value)
                return value
            def advance(name):
                (evidence / (name + ".continue")).touch()
            def change(**fields):
                return call("editor.viewport.set", dict(guard=call("editor.viewport.query")["guard"], **fields))
            def finish(ticket):
                deadline = time.monotonic() + 120
                while True:
                    job = call("jobs.wait", dict(id=ticket["job_id"], timeout_ms=100))["job"]
                    if job["state"] in ("succeeded", "failed", "cancelled"):
                        assert job["state"] == "succeeded", job
                        assert job["result"]["voxel_count"] == 1
                        for field in ("document_id", "scene_id", "revision", "frame", "width", "height"):
                            assert job["result"][field] == ticket[field]
                        report["captures"].append(job["result"])
                        return job["result"]
                    assert time.monotonic() < deadline
            def capture(name, reference, **fields):
                params = dict(guard=guard(call("scene.query")["state"]), output=name,
                    width=reference["presentation"]["width"], height=reference["presentation"]["height"],
                    camera=reference["camera"], validation="required")
                params.update(fields)
                return call("render.capture", params)
            def reject(method, params):
                try:
                    call(method, params)
                except RpcError:
                    return
                raise AssertionError("Invalid request accepted: " + method)

            base = checkpoint("base")
            assert base["voxel_count"] == 1 and base["center_pick"] and not base["dirty"]
            assert base["bounds"] == {"min": [-.5, -.5, 0.0], "max": [1.5, .5, 1.0]}
            initial_scene = call("scene.query")["state"]
            descriptors = {name: call("commands.describe", dict(name=name)) for name in ("editor.viewport.set", "render.capture")}
            (directory / "descriptions.json").write_text(json.dumps(descriptors, indent=2))
            before = call("editor.viewport.query")
            reject("editor.viewport.set", dict(guard=before["guard"], voxel_light_direction=[0, 0, 0]))
            assert call("editor.viewport.query")["guard"] == before["guard"]
            original = (root / "assets/solid.dkvoxel").read_bytes()
            ticket = capture("frozen.ppm", base)
            assert ticket["provenance"]["asset_snapshot"] == "accepted" and ticket["provenance"]["asset_cpu_bytes"] > 0
            (root / "assets/solid.dkvoxel").write_text("broken after acceptance")
            finish(ticket)
            assert (root / "frozen.ppm").read_bytes() == (evidence / "base.ppm").read_bytes()
            reject("render.capture", dict(guard=guard(call("scene.query")["state"]), output="frozen.ppm", camera=base["camera"]))
            assert (root / "frozen.ppm").read_bytes() == (evidence / "base.ppm").read_bytes()
            (root / "assets/solid.dkvoxel").write_bytes(original)

            advance("base"); gui = checkpoint("gui")
            assert gui["settings"]["voxel_shading"] == "directional" and gui["pixels"] != base["pixels"]
            advance("gui"); console = checkpoint("console")
            assert console["settings"]["voxel_shading"] == "unlit" and console["settings"]["voxel_irradiance"] == [2, 2, 2]
            assert console["console_history"] > 0 and console["pixels"] == base["pixels"]
            assert call("scene.query")["state"] == initial_scene
            advance("console"); selected = checkpoint("selected")
            assert selected["selection"] == base["center_pick"]
            advance("selected"); moved = checkpoint("moved")
            assert moved["scene_revision"] > selected["scene_revision"] and moved["undo"] == selected["undo"] + 1
            assert moved["pixels"] != selected["pixels"] and moved["dirty"]
            advance("moved"); undone = checkpoint("undo")
            assert undone["pixels"] == base["pixels"] and undone["redo"] == 1
            advance("undo"); redone = checkpoint("redo")
            assert redone["pixels"] == moved["pixels"] and redone["redo"] == 0
            advance("redo"); framed = checkpoint("framed")
            assert framed["scene_revision"] == redone["scene_revision"] and framed["pixels"] != redone["pixels"]
            call("editor.camera.set", dict(guard=call("editor.camera.query")["guard"], projection="orthographic", ortho_height=3))
            advance("framed"); ortho = checkpoint("orthographic")
            finish(capture("ortho.ppm", ortho))
            assert (root / "ortho.ppm").read_bytes() == (evidence / "orthographic.ppm").read_bytes()
            (root / "assets/solid.dkvoxel").write_text("broken reload")
            call("scene.load", dict(manifest="project.json", guard=guard(call("scene.query")["state"])))
            advance("orthographic"); failed = checkpoint("failed")
            assert not failed["presentation"]["current"] and failed["pixels"] == ortho["pixels"]
            (root / "assets/solid.dkvoxel").write_bytes(original)
            call("scene.load", dict(manifest="project.json", guard=guard(call("scene.query")["state"])))
            advance("failed"); recovered = checkpoint("recovered")
            assert recovered["presentation"]["current"]
            change(voxel_shading="directional", voxel_light_direction=[0, 0, -1], voxel_irradiance=[2, 2, 2], raster_exposure=2)
            advance("recovered"); ipc = checkpoint("ipc")
            finish(capture("directional.ppm", ipc, voxel_shading="directional", voxel_light_direction=[0, 0, -1], voxel_irradiance=[2, 2, 2], exposure=2))
            assert (root / "directional.ppm").read_bytes() == (evidence / "ipc.ppm").read_bytes()
            assert ipc["ui_errors"] == 1  # Failed import is surfaced once; recovery is usable.
            original_project = (root / "project.json").read_bytes()
            original_scene = (root / "scene.json").read_bytes()
            mesh_id, mesh_entity = str(uuid4()), str(uuid4())
            mesh = json.loads((args.mesh_fixture / "back.gltf").read_text())
            mesh["materials"][0]["doubleSided"] = True
            mesh["materials"][0]["pbrMetallicRoughness"]["metallicFactor"] = 0
            (root / "assets/back.gltf").write_text(json.dumps(mesh))
            shutil.copyfile(args.mesh_fixture / "quad.bin", root / "assets/quad.bin")
            project = json.loads(original_project)
            project["assets"].append(dict(id=mesh_id, kind="mesh", path="assets/back.gltf"))
            (root / "project.json").write_text(json.dumps(project))
            mixed = json.loads(original_scene)
            entity = copy.deepcopy(mixed["entities"][0])
            entity["components"]["dk.Identity"]["id"] = mesh_entity
            entity["components"]["dk.Transform"]["translation"] = [0, 0, -.5]
            entity["components"]["dk.AssetReferences"]["items"] = [dict(id=mesh_id, kind="mesh")]
            mixed["entities"].append(entity)
            (root / "scene.json").write_text(json.dumps(mixed))
            call("scene.load", dict(manifest="project.json", guard=guard(call("scene.query")["state"])))
            call("editor.camera.reset", dict(guard=call("editor.camera.query")["guard"], defaults=True))
            change(voxel_shading="unlit", raster_exposure=1)
            advance("ipc"); mesh_front = checkpoint("mesh_front")
            assert mesh_front["center_pick"] == mesh_entity
            finish(capture("mesh-front.ppm", mesh_front))
            assert (root / "mesh-front.ppm").read_bytes() == (evidence / "mesh_front.ppm").read_bytes()
            call("entity.set_transform", dict(guard=guard(call("scene.query")["state"]), id=mesh_entity,
                transform=dict(translation=[0, 0, 2], rotation=[0, 0, 0, 1], scale=[1, 1, 1])))
            advance("mesh_front"); voxel_front = checkpoint("voxel_front")
            assert voxel_front["center_pick"] == base["center_pick"] and voxel_front["pixels"] != mesh_front["pixels"]
            finish(capture("voxel-front.ppm", voxel_front))
            assert (root / "voxel-front.ppm").read_bytes() == (evidence / "voxel_front.ppm").read_bytes()
            (root / "project.json").write_bytes(original_project)
            (root / "scene.json").write_bytes(original_scene)
            call("runtime.shutdown")
            assert process.wait(timeout=30) == 0
            logs = (directory / "stdout.log").read_text(errors="replace")
            assert "validation errors=0" in logs and "liveAllocations=0" in logs

            # The same capture command works with no Editor or viewport attached.
            runner_endpoint = endpoint + "-runner"
            process = subprocess.Popen([str(args.runner), "--project-root", str(root), "--pipe", runner_endpoint],
                stdin=subprocess.DEVNULL, stdout=out, stderr=err, creationflags=subprocess.CREATE_NO_WINDOW)
            client = Client(runner_endpoint, args.client, timeout=30)
            call("scene.load", dict(manifest="project.json"))
            assert not any(c["name"].startswith("editor.") for c in call("commands.list"))
            finish(capture("standalone.ppm", ipc, voxel_shading="directional", voxel_light_direction=[0, 0, -1], voxel_irradiance=[2, 2, 2], exposure=2))
            assert (root / "standalone.ppm").read_bytes() == (root / "directional.ppm").read_bytes()
            example = subprocess.run([sys.executable, str(args.example), "--endpoint", runner_endpoint, "--client", str(args.client),
                "--width", str(base["presentation"]["width"]), "--height", str(base["presentation"]["height"]), "--output", "example.ppm"],
                capture_output=True, text=True, timeout=120, creationflags=subprocess.CREATE_NO_WINDOW)
            assert example.returncode == 0, example.stderr
            assert (root / "example.ppm").read_bytes() == (evidence / "base.ppm").read_bytes()
            report["example"] = json.loads(example.stdout)
            current = guard(call("scene.query")["state"])
            reject("render.capture", dict(guard=current, output="path.ppm", camera=base["camera"], renderer="pathtrace",
                pathtrace=dict(version=1, target_spp=1, surface_only=True)))
            reject("render.sequence", dict(guard=current, output="sequence.json", samples=[dict(camera=base["camera"], time_seconds=0)]))
            environment = call("environment.query")["environment"]
            environment["enabled"] = True
            call("environment.set", dict(guard=current, environment=environment))
            reject("render.capture", dict(guard=guard(call("scene.query")["state"]), output="environment.ppm", camera=base["camera"]))
            finish(capture("inspection.ppm", base, unlit_inspection=True))
            assert (root / "inspection.ppm").read_bytes() == (evidence / "base.ppm").read_bytes()
            assert call("environment.query")["environment"]["enabled"]  # Rendering did not edit the Scene.
            call("runtime.shutdown"); assert process.wait(timeout=30) == 0
            report["status"] = "passed"
        finally:
            if process is not None and process.poll() is None:
                process.kill(); process.wait(timeout=15)
            (directory / "summary.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
    print("Voxel Editor/Console/IPC/capture acceptance passed:", directory)


if __name__ == "__main__":
    main()
