"""M14.6: real environment UI, IPC, frozen captures, persistence and executable scripts."""
import argparse
import copy
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys
import time
from uuid import uuid4

from decker import Client, guard


def main():
    parser = argparse.ArgumentParser()
    for name in ("editor", "client", "runner", "fixtures", "python-example", "luau-example", "work-dir"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--clouds", action="store_true")
    args = parser.parse_args()
    directory = args.work_dir / ("environment-" + uuid4().hex)
    root = directory / "project"
    shutil.copytree(args.fixtures, root)
    # Migrate a disposable fixture, keeping geometry safely above the spherical ground.
    seed = json.loads((Path(__file__).parents[2] / "examples/automation/environment.jsonl").read_text().splitlines()[1])
    environment = seed["params"]["environment"]
    environment["enabled"] = False
    environment["atmosphere"]["planet_center_m"] = [0, -environment["atmosphere"]["ground_radius_m"]-1000, 0]
    environment["sun"]["direction"] = [0, 0, -1]
    environment["sun"]["irradiance"] = [4, 4, 4]
    scene = json.loads((root / "scene.json").read_text())
    scene.update(version=2, environment=environment)
    if args.clouds:
        for entity in scene["entities"]:
            if entity["components"]["dk.Hierarchy"]["parent"] is None:
                entity["components"]["dk.Transform"]["translation"][1] += 2
        (root / ".dk-cloud-integration").touch()
    (root / "scene.json").write_text(json.dumps(scene), encoding="utf-8")
    # Legacy unlit ignores sidedness. Our camera sees the back of these fixture quads;
    # mark the disposable materials double-sided so the lit test includes visible receivers.
    for asset in (root / "assets").glob("*.gltf"):
        gltf = json.loads(asset.read_text())
        for material in gltf["materials"]:
            material["doubleSided"] = True
        asset.write_text(json.dumps(gltf), encoding="utf-8")
    (root / ".dk-editor-smoke").touch()
    evidence = root / ".dk-environment"
    endpoint = "dk-environment-" + uuid4().hex
    report = {"status": "failed", "checkpoints": [], "captures": []}
    command = [args.editor, "--background", "--root", root, "--pipe", endpoint, "--fixture-camera", "--validation",
               "--environment-consistency-smoke", "--screenshot", directory / "workbench.ppm"]
    with (directory / "stdout.log").open("wb") as out, (directory / "stderr.log").open("wb") as err:
        process = subprocess.Popen([str(x) for x in command], stdin=subprocess.DEVNULL, stdout=out, stderr=err,
                                   creationflags=subprocess.CREATE_NO_WINDOW)
        client = Client(endpoint, args.client, timeout=30)

        def checkpoint(name):
            path = evidence / (name + ".json")
            end = time.monotonic() + 100
            while True:
                try:
                    value = json.loads(path.read_text())
                    break
                except (FileNotFoundError, PermissionError):
                    # Windows may briefly deny opening the just-renamed atomic file.
                    pass
                if process.poll() is not None or time.monotonic() >= end:
                    raise RuntimeError("Editor did not present " + name + ": " + (directory / "stderr.log").read_text())
                time.sleep(.01)
            query = client.call("environment.query").value
            assert query["state"] == value["state"], (query, value)
            if not value["pending"]:
                assert query["environment"] == value["environment_draft"]
            report["checkpoints"].append(value)
            return value

        def advance(name):
            (evidence / (name + ".continue")).touch()

        def edit(irradiance):
            query = client.call("environment.query").value
            query["environment"]["sun"]["irradiance"] = irradiance
            client.call("environment.set", {"guard": guard(query["state"]), "environment": query["environment"]})

        def params(point, name):
            view = point["viewport"]
            return {"guard": guard(point["state"]), "output": name+".ppm", "width": view["width"], "height": view["height"],
                    "camera": view["camera"], "exposure": 1, "profile": "unlit_preview", "validation": "required",
                    "render_options": view["render_options"], "time_seconds": view["time_seconds"], "warmup_frames": view["warmup_frames"]}

        def validate_capture(point, ticket, job):
            assert job["state"] == "succeeded", job
            result = job["result"]
            expected = copy.deepcopy(ticket["provenance"])
            expected["shader_fingerprint"] = result["provenance"]["shader_fingerprint"]
            assert result["provenance"] == expected
            assert re.fullmatch(r"fnv1a64:[0-9a-f]{16}", expected["shader_fingerprint"])
            assert expected["time_seconds"] == 0 and expected["exposure"] == 1
            assert expected["environment"] == point["environment_draft"]
            assert expected["camera"] == point["viewport"]["camera"]
            for key in ("document_id", "scene_id", "revision", "width", "height", "frame", "output"):
                assert result[key] == ticket[key]
            assert (root / result["output"]).read_bytes() == (root / point["viewport"]["image"]).read_bytes(), point["checkpoint"]
            report["captures"].append(job)

        try:
            initial = checkpoint("initial")
            advance("initial")
            gui = checkpoint("gui")
            if args.clouds:
                assert gui["environment_draft"]["clouds"]["enabled"] and gui["environment_draft"]["clouds"]["seed"] == 1729
                assert gui["viewport"]["shadow_generation"] > 0 and gui["viewport"]["warmup_frames"] == 32
                assert gui["viewport"]["history_reset"] == "none"
            # Require colored surface receivers; comparing two sky-only frames is insufficient.
            pixels = (root / gui["viewport"]["image"]).read_bytes().split(b"\n", 3)[3]
            assert any((r > g+30 and r > b+30) or (b > r+30 and b > g+30)
                       for r, g, b in zip(pixels[0::3], pixels[1::3], pixels[2::3])), "No lit fixture receivers"
            # Submit before mutation; completion must describe and reproduce the old GUI scene.
            ticket = client.call("render.capture", params(gui, "frozen-gui")).value
            edit([1, 2, 3])
            while True:
                waited = client.call("jobs.wait", {"id": ticket["job_id"], "timeout_ms": 1000}).value
                if not waited["timed_out"]:
                    break
            validate_capture(gui, ticket, waited["job"])
            advance("gui")
            checkpoint("external"); advance("external")
            checkpoint("draft")
            edit([3, 1, 2]); advance("draft")
            for name in ("conflict", "undo", "redo", "saved", "reloaded"):
                point = checkpoint(name)
                if name == "reloaded":
                    capture = client.capture(params(point, "reloaded-capture"), timeout=120)
                    validate_capture(point, capture.submission.value, capture.job)
                    persisted = json.loads((root / "scene.json").read_text())
                    assert persisted["environment"] == point["environment_draft"]
                    # Run the repository Python example against the real editor Runtime.
                    script_args = [sys.executable, str(args.python_example), "--pipe", endpoint, "--ctl", str(args.client)]
                    if not args.clouds:
                        script_args += ["--project-root", str(root)]
                    script = subprocess.run(script_args, capture_output=True, text=True, timeout=150,
                                            creationflags=subprocess.CREATE_NO_WINDOW)
                    assert script.returncode == 0, (script.stdout, script.stderr)
                    report["python"] = json.loads(script.stdout)
                advance(name)
            gizmo = checkpoint("gizmo")
            capture = client.capture(params(gizmo, "gizmo-capture"), timeout=120)
            validate_capture(gizmo, capture.submission.value, capture.job)
            advance("gizmo")
            assert process.wait(timeout=60) == 0, (directory / "stderr.log").read_text()
            log = (directory / "stdout.log").read_text()
            assert "validation errors=0 warnings=0 liveAllocations=0" in log, log
            assert not (directory / "stderr.log").read_text()
            # GUI gizmo edited only memory. The script reopens the saved, unmodified geometry.
            script = subprocess.run([str(args.runner), "--project-root", str(root), "--script", str(args.luau_example),
                                     "--script-timeout-ms", "120000"], capture_output=True, text=True, timeout=130,
                                    creationflags=subprocess.CREATE_NO_WINDOW)
            assert script.returncode == 0, (script.stdout, script.stderr)
            assert not script.stderr, script.stderr
            if args.clouds:
                a = json.loads((root / "cloud-python.json").read_text())
                b = json.loads((root / "cloud-luau.json").read_text())
                assert [(f["rgba8"], f["linear_rgba"]) for f in a["frames"]] == [(f["rgba8"], f["linear_rgba"]) for f in b["frames"]]
            else:
                assert (root / "environment-python.ppm").read_bytes() == (root / "environment-luau.ppm").read_bytes()
            report["luau"] = "passed; identical images/linear values to Python example"
            report["status"] = "passed"
        finally:
            if process.poll() is None:
                process.kill(); process.wait(timeout=10)
            (directory / "report.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
    print("Environment GUI/IPC/frozen capture/Python/Luau passed: " + str(directory))


if __name__ == "__main__":
    main()
