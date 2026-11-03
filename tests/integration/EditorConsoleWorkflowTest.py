"""M22.11: actual completion -> command -> GUI field -> IPC/image confirmation."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys
import time
from uuid import uuid4
from decker import Client, RpcError


def main():
    parser = argparse.ArgumentParser()
    for key in ("editor", "client", "runner", "fixtures", "work-dir", "example"):
        parser.add_argument("--" + key, type=Path, required=True)
    args = parser.parse_args()
    directory = args.work_dir / ("console-workflow-" + uuid4().hex)
    root = directory / "project"
    shutil.copytree(args.fixtures, root)
    for asset in (root / "assets").glob("*.gltf"):
        value = json.loads(asset.read_text())
        for material in value["materials"]:
            material["doubleSided"] = True
        asset.write_text(json.dumps(value), encoding="utf-8")
    (root / ".dk-editor-smoke").touch()
    scene_hash = hashlib.sha256((root / "scene.json").read_bytes()).hexdigest()
    endpoint = "dk-console-flow-" + uuid4().hex
    report = {"status": "failed", "checkpoints": []}
    runner_root = directory / "runner"
    runner_root.mkdir()
    batch = directory / "runner.jsonl"
    batch.write_text(json.dumps(dict(jsonrpc="2.0", id=1, method="commands.list")) + "\n")
    runner = subprocess.run([str(args.runner), "--project-root", str(runner_root), "--batch", str(batch)], capture_output=True, text=True, timeout=20)
    assert runner.returncode == 0, runner.stderr
    commands = json.loads(runner.stdout)["result"]["value"]
    assert not any(entry["name"].startswith("editor.") for entry in commands)
    report["runner_commands"] = len(commands)
    process = None
    with (directory / "stdout.log").open("wb") as out, (directory / "stderr.log").open("wb") as err:
        try:
            process = subprocess.Popen([str(args.editor), "--background", "--root", str(root), "--pipe", endpoint,
                "--fixture-camera", "--validation", "--console-workflow-smoke", "--screenshot", str(directory / "workflow.ppm")],
                stdin=subprocess.DEVNULL, stdout=out, stderr=err, creationflags=subprocess.CREATE_NO_WINDOW)
            client = Client(endpoint, args.client, timeout=10)
            evidence = root / ".dk-console-workflow"

            def query():
                return client.call("editor.camera.query").value

            def checkpoint(name):
                deadline = time.monotonic() + 60
                while True:
                    try:
                        value = json.loads((evidence / (name + ".json")).read_text(encoding="utf-8"))
                        break
                    except (FileNotFoundError, PermissionError):
                        pass
                    if process.poll() is not None or time.monotonic() > deadline:
                        raise RuntimeError("Missing workflow checkpoint " + name + ": " + (directory / "stderr.log").read_text(errors="replace"))
                    time.sleep(.02)
                camera, view = query(), client.call("editor.viewport.query").value
                assert camera["guard"] == value["camera"]["guard"] == view["guard"]
                assert camera["settings"] == value["camera"]["settings"]
                assert view["settings"] == value["viewport"]["settings"]
                assert value["scene"] == dict(revision=1, dirty=False, undo=0, redo=0)
                assert camera["presentation"]["current"] and view["presentation"]["current"]
                assert value["published_camera_revision"] == camera["render_revision"] == camera["presentation"]["camera_revision"]
                if report["checkpoints"]:
                    assert value["simulation_camera_revision"] == report["checkpoints"][0]["simulation_camera_revision"]
                pixels = (evidence / (name + ".ppm")).read_bytes().split(b"\n", 3)[3]
                assert len({pixels[i:i+3] for i in range(0, len(pixels), 3)}) >= 3, "missing nonuniform image: " + name
                report["checkpoints"].append(value)
                return value

            def advance(name):
                # Explicitly copy the observed guard to the native driver acting as a user.
                temporary = evidence / (name + ".tmp")
                temporary.write_text(json.dumps({"guard": query()["guard"]}), encoding="utf-8")
                temporary.replace(evidence / (name + ".continue"))

            def example():
                return subprocess.run([sys.executable, str(args.example), "--pipe", endpoint, "--ctl", str(args.client),
                    "--target-spp", "2", "--yaw-deg", "5", "--pitch-deg", "2", "--timeout", "30"],
                    capture_output=True, text=True, encoding="utf-8", timeout=40)

            base = checkpoint("baseline")
            commands = client.call("commands.list").value
            assert all(set(entry) == {"name", "description", "effect", "undoable"} for entry in commands)
            assert len([entry for entry in commands if entry["name"].startswith("editor.")]) == 12
            report["editor_commands"] = len(commands)
            descriptions = {method: client.call("commands.describe", {"name": method}).value for method in
                ("editor.camera.set", "editor.camera.orbit", "editor.viewport.set", "simulation.run", "entity.set_transform")}
            assert all(d["help"]["version"] == 1 for d in descriptions.values())
            assert descriptions["editor.camera.set"]["help"]["fields"]["/properties/fov_y_deg"]["unit"] == "deg"
            assert descriptions["editor.viewport.set"]["parameters"]["properties"]["renderer"]["enum"] == ["raster", "pathtrace"]
            (directory / "descriptions.json").write_text(json.dumps(descriptions, indent=2), encoding="utf-8")
            advance("baseline")
            lens = checkpoint("console_lens")
            assert lens["camera"]["settings"]["fov_y_deg"] == 35 and lens["console_history"] == 1
            assert lens["pixels"] != base["pixels"] and lens["camera"]["guard"]["revision"] == base["camera"]["guard"]["revision"] + 1
            advance("console_lens")
            panel = checkpoint("panel_sync")
            assert float(panel["gui_value"]) == 35 and panel["camera"]["guard"] == lens["camera"]["guard"]
            blocked = example()
            report["blocked_example"] = json.loads(blocked.stderr)
            assert blocked.returncode == 1 and not report["blocked_example"]["accepted"]
            assert "blocked" in report["blocked_example"]["error"]
            assert query()["guard"] == panel["camera"]["guard"] and query()["settings"] == panel["camera"]["settings"]
            advance("panel_sync")
            gui = checkpoint("gui_lens")
            assert gui["camera"]["settings"]["fov_y_deg"] == 45 and gui["pixels"] != lens["pixels"]
            try:
                client.call("editor.camera.set", {"guard": lens["camera"]["guard"], "fov_y_deg": 80})
            except RpcError:
                report["stale_guard_rejected"] = True
            else:
                raise AssertionError("Stale guard accepted after GUI Apply")
            assert query()["guard"] == gui["camera"]["guard"] and query()["settings"] == gui["camera"]["settings"]
            advance("gui_lens")
            legacy = checkpoint("legacy_json")
            assert legacy["camera"]["settings"] == lens["camera"]["settings"] and legacy["pixels"] == lens["pixels"]
            assert legacy["console_history"] == 2
            advance("legacy_json")
            path = checkpoint("path_console")
            assert path["viewport"]["settings"]["renderer"] == "pathtrace" and path["console_history"] == 3
            assert path["viewport"]["settings"]["target_spp"] == path["viewport"]["presentation"]["spp"] == 2
            advance("path_console")
            path_panel = checkpoint("path_panel")
            assert int(path_panel["gui_value"]) == 2 and path_panel["camera"]["guard"] == path["camera"]["guard"]
            applied = example()
            assert applied.returncode == 0, applied.stderr
            report["example"] = json.loads(applied.stdout)
            assert report["example"]["status"] == "passed" and len(report["example"]["accepted"]) == 2
            advance("path_panel")
            final = checkpoint("ipc_example")
            assert final["viewport"]["settings"]["direct_lighting"] == "restir_di"
            assert final["camera"]["guard"] == report["example"]["camera"]["guard"] and final["pixels"] != path["pixels"]
            assert final["console_history"] == 3
            advance("ipc_example")
            assert process.wait(timeout=20) == 0, (directory / "stderr.log").read_text(errors="replace")
            logs = (directory / "stdout.log").read_text(errors="replace")
            assert "validation errors=0 warnings=0 liveAllocations=0" in logs
            assert hashlib.sha256((root / "scene.json").read_bytes()).hexdigest() == scene_hash
            assert (directory / "workflow.ppm").stat().st_size > 100000
            report["status"] = "passed"
        finally:
            if process is not None and process.poll() is None:
                process.kill(); process.wait(timeout=15)
            (directory / "summary.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
    print("Console/Settings/GUI/IPC workflow and Python example passed:", directory)


if __name__ == "__main__":
    main()
