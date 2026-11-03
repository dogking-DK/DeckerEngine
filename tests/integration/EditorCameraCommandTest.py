"""M22.9 guarded Edit camera changes through real GUI, Console and IPC."""
import argparse
import json
from pathlib import Path
import shutil
import subprocess
import time
from uuid import uuid4
from decker import Client, RpcError


def main():
    parser = argparse.ArgumentParser()
    for key in ("editor", "client", "runner", "fixtures", "work-dir"):
        parser.add_argument("--" + key, type=Path, required=True)
    args = parser.parse_args()
    directory = args.work_dir / ("camera-commands-" + uuid4().hex)
    root = directory / "project"
    shutil.copytree(args.fixtures, root)
    for asset in (root / "assets").glob("*.gltf"):
        value = json.loads(asset.read_text())
        for material in value["materials"]:
            material["doubleSided"] = True
        asset.write_text(json.dumps(value), encoding="utf-8")
    (root / ".dk-editor-smoke").touch()
    endpoint = "dk-camera-" + uuid4().hex
    report = {"status": "failed", "checkpoints": []}
    runner_root = directory / "runner"
    runner_root.mkdir()
    requests = directory / "runner.jsonl"
    requests.write_text(json.dumps(dict(jsonrpc="2.0", id=1, method="commands.list")) + "\n")
    runner = subprocess.run([str(args.runner), "--project-root", str(runner_root), "--batch", str(requests)], capture_output=True, text=True, timeout=20)
    assert runner.returncode == 0, runner.stderr
    commands = json.loads(runner.stdout)["result"]["value"]
    assert not any(d["name"].startswith("editor.") for d in commands)
    report["runner_commands"] = len(commands)
    process = None
    with (directory / "stdout.log").open("wb") as out, (directory / "stderr.log").open("wb") as err:
        try:
            process = subprocess.Popen([str(args.editor), "--background", "--root", str(root), "--pipe", endpoint,
                "--fixture-camera", "--validation", "--camera-command-smoke"], stdin=subprocess.DEVNULL, stdout=out, stderr=err,
                creationflags=subprocess.CREATE_NO_WINDOW)
            client = Client(endpoint, args.client, timeout=10)
            evidence = root / ".dk-camera"

            def query():
                return client.call("editor.camera.query").value

            def checkpoint(name):
                end = time.monotonic() + 90
                while True:
                    try:
                        value = json.loads((evidence / (name + ".json")).read_text(encoding="utf-8"))
                        break
                    except (FileNotFoundError, PermissionError):
                        pass
                    if process.poll() is not None or time.monotonic() > end:
                        raise RuntimeError("Missing camera checkpoint " + name + ": " + (directory / "stderr.log").read_text(errors="replace"))
                    time.sleep(.02)
                q = query()
                assert q["guard"] == value["guard"] and q["settings"] == value["settings"]
                native = value["native"]
                assert native["scene_revision"] == 1 and not native["dirty"] and native["undo"] == 0
                if name != "active_close":
                    assert value["presentation"]["current"]
                    assert native["published_camera_revision"] == value["render_revision"] == native["render_revision"]
                    assert native["published_eye"] == value["settings"]["eye"]
                if report["checkpoints"]:
                    assert native["simulation_camera_revision"] == report["checkpoints"][0]["native"]["simulation_camera_revision"]
                report["checkpoints"].append(value)
                return value

            def advance(name):
                (evidence / (name + ".continue")).touch()

            def camera(action, **params):
                return client.call("editor.camera." + action, dict(guard=query()["guard"], **params)).value

            def viewport(**params):
                return client.call("editor.viewport.set", dict(guard=query()["guard"], **params)).value

            def reject(method, params):
                before = query()
                try:
                    client.call(method, params)
                except RpcError:
                    pass
                else:
                    raise AssertionError("Invalid request accepted: " + method)
                after = query()
                assert after["guard"] == before["guard"] and after["settings"] == before["settings"]

            base = checkpoint("raster")
            commands = client.call("commands.list").value
            names = ("query", "set", "orbit", "pan", "zoom", "frame", "reset")
            assert {d["name"] for d in commands if d["name"].startswith("editor.camera.")} == {"editor.camera." + x for x in names}
            descriptions = {name: client.call("commands.describe", dict(name="editor.camera." + name)).value for name in names}
            assert all(not d["undoable"] and d["help"]["version"] == 1 for d in descriptions.values())
            report["editor_commands"] = len(commands)
            (directory / "descriptions.json").write_text(json.dumps(descriptions, indent=2), encoding="utf-8")
            accepted = camera("orbit", yaw_deg=15, pitch_deg=20)
            assert not accepted["presentation"]["current"]
            advance("raster")
            orbit = checkpoint("orbit_ipc")
            assert orbit["pixels"] != base["pixels"] and orbit["render_revision"] > base["render_revision"]
            reject("editor.camera.set", dict(guard=base["guard"], move_speed=2))
            advance("orbit_ipc")
            ortho = checkpoint("orthographic_console")
            assert ortho["settings"]["projection"] == "orthographic" and ortho["settings"]["standard_view"] == "front"
            assert ortho["native"]["console_history"] == 1
            pixels = (evidence / "orthographic_console.ppm").read_bytes().split(b"\n", 3)[3]
            assert len({pixels[i:i+3] for i in range(0, len(pixels), 3)}) >= 3
            advance("orthographic_console")
            lens = checkpoint("lens_gui")
            assert lens["settings"]["ortho_height"] == 4 and lens["pixels"] != ortho["pixels"]
            advance("lens_gui")
            draft = checkpoint("draft_gui")
            assert draft["guard"] == lens["guard"] and draft["settings"] == lens["settings"] and draft["blocked_reason"]
            reject("editor.camera.set", dict(guard=draft["guard"], ortho_height=5))
            reject("editor.viewport.set", dict(guard=draft["guard"], renderer="pathtrace"))
            advance("draft_gui")
            cancelled = checkpoint("cancel_gui")
            assert cancelled["guard"] == lens["guard"] and not cancelled["blocked_reason"]
            scene = client.call("scene.query").value
            entity = next(e["id"] for e in scene["entities"] if e["assets"])
            camera("frame", entity=entity); advance("cancel_gui")
            framed = checkpoint("frame_ipc")
            assert framed["render_revision"] > lens["render_revision"]
            advance("frame_ipc")
            reset = checkpoint("reset_gui")
            assert reset["settings"]["eye"] == base["settings"]["eye"]
            assert reset["settings"]["projection"] == "orthographic"
            camera("set", projection="perspective")
            viewport(renderer="pathtrace", target_spp=2); advance("reset_gui")
            path = checkpoint("path_ipc")
            advance("path_ipc")
            speed = checkpoint("speed_gui")
            assert speed["settings"]["move_speed"] == 9
            assert speed["render_revision"] == path["render_revision"] and speed["pixels"] == path["pixels"]
            assert speed["native"]["path_generation"] == path["native"]["path_generation"]
            assert camera("set", move_speed=9)["guard"] == speed["guard"]
            advance("speed_gui")
            pan = checkpoint("pan_console")
            assert pan["native"]["console_history"] == 2 and pan["pixels"] != speed["pixels"]
            assert pan["native"]["path_generation"] > speed["native"]["path_generation"]
            advance("pan_console")
            gesture = checkpoint("gesture_gui")
            assert gesture["native"]["navigating"] and gesture["blocked_reason"]
            reject("editor.camera.reset", dict(guard=gesture["guard"]))
            reject("editor.viewport.set", dict(guard=gesture["guard"], renderer="raster"))
            advance("gesture_gui")
            released = checkpoint("released")
            assert not released["blocked_reason"]
            reject("editor.camera.set", dict(guard=released["guard"], move_speed=12, fov_y_deg=179))
            reject("editor.camera.set", dict(guard=released["guard"], projection="orthographic", navigation_mode="fly"))
            scene = client.call("scene.query").value["state"]
            client.call("scene.load", dict(manifest="project.json", guard={k: scene[k] for k in ("document_id", "revision")}))
            reject("editor.camera.zoom", dict(guard=released["guard"], steps=1))
            viewport(target_spp=1000000); advance("released")
            checkpoint("active_close")
            client.call("runtime.shutdown")
            assert process.wait(timeout=30) == 0, (directory / "stderr.log").read_text(errors="replace")
            logs = (directory / "stdout.log").read_text(errors="replace")
            assert "validation errors=0 warnings=0 liveAllocations=0" in logs
            report["status"] = "passed"
        finally:
            if process is not None and process.poll() is None:
                process.kill(); process.wait(timeout=15)
            (directory / "summary.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
    print("Camera GUI/Console/IPC passed:", directory)


if __name__ == "__main__":
    main()
