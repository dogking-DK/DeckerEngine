"""M22.8: native GUI, Console and IPC operate one guarded Edit viewport."""
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
    directory = args.work_dir / ("viewport-commands-" + uuid4().hex)
    root = directory / "project"
    shutil.copytree(args.fixtures, root)
    # This fixture camera sees the backs of the planes; use visible two-sided geometry,
    # as in the existing path preview acceptance, so pixel checks exercise surfaces.
    for asset in (root / "assets").glob("*.gltf"):
        value = json.loads(asset.read_text())
        for material in value["materials"]:
            material["doubleSided"] = True
        asset.write_text(json.dumps(value), encoding="utf-8")
    (root / ".dk-editor-smoke").touch()
    endpoint = "dk-viewport-" + uuid4().hex
    report = {"status": "failed", "checkpoints": []}
    runner_root = directory / "runner"
    runner_root.mkdir()
    requests = directory / "runner.jsonl"
    requests.write_text(json.dumps(dict(jsonrpc="2.0", id=1, method="commands.list"))+"\n")
    runner = subprocess.run([str(args.runner), "--project-root", str(runner_root), "--batch", str(requests)], capture_output=True, text=True, timeout=20)
    assert runner.returncode == 0, runner.stderr
    commands = json.loads(runner.stdout)["result"]["value"]
    assert not any(d["name"].startswith("editor.") for d in commands)
    report["runner_commands"] = len(commands)
    process = None
    with (directory / "stdout.log").open("wb") as out, (directory / "stderr.log").open("wb") as err:
        try:
            process = subprocess.Popen([str(args.editor), "--background", "--root", str(root), "--pipe", endpoint,
                "--fixture-camera", "--validation", "--viewport-command-smoke"], stdin=subprocess.DEVNULL, stdout=out, stderr=err,
                creationflags=subprocess.CREATE_NO_WINDOW)
            client = Client(endpoint, args.client, timeout=10)
            evidence = root / ".dk-viewport"

            def query():
                return client.call("editor.viewport.query").value

            def checkpoint(name):
                end = time.monotonic() + 100
                while True:
                    try:
                        value = json.loads((evidence / (name + ".json")).read_text(encoding="utf-8"))
                        break
                    except (FileNotFoundError, PermissionError):
                        pass
                    if process.poll() is not None or time.monotonic() > end:
                        raise RuntimeError("Missing checkpoint " + name + ": " + (directory / "stderr.log").read_text(errors="replace"))
                    time.sleep(.02)
                q = query()
                assert q["guard"] == value["guard"] and q["settings"] == value["settings"]
                assert value["native"]["scene_revision"] == 1 and value["native"]["undo"] == 0 and not value["native"]["dirty"]
                report["checkpoints"].append(value)
                return value

            def advance(name):
                (evidence / (name + ".continue")).touch()

            def change(**settings):
                return client.call("editor.viewport.set", dict(guard=query()["guard"], **settings)).value

            def action(name):
                return client.call("editor.viewport." + name, dict(guard=query()["guard"])).value

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
            editor_commands = client.call("commands.list").value
            assert {d["name"] for d in editor_commands if d["name"].startswith("editor.")} == {
                "editor.viewport." + name for name in ("query", "set", "pause", "resume", "reset")}
            report["editor_commands"] = len(editor_commands)
            desc = {name: client.call("commands.describe", dict(name="editor.viewport."+name)).value for name in ("query", "set", "pause", "resume", "reset")}
            assert all(not d["undoable"] and d["help"]["version"] == 1 for d in desc.values())
            (directory / "descriptions.json").write_text(json.dumps(desc, indent=2), encoding="utf-8")
            accepted = change(renderer="pathtrace", target_spp=2, seed=123)
            assert not accepted["presentation"]["current"]
            advance("raster")
            path = checkpoint("path_ipc")
            assert path["presentation"]["current"] and path["settings"]["target_spp"] == 2
            advance("path_ipc")
            exposure = checkpoint("exposure_gui")
            assert exposure["settings"]["exposure_ev"] == 1 and exposure["native"]["generation"] == path["native"]["generation"]
            assert exposure["pixels"] != path["pixels"]
            advance("exposure_gui")
            paused = checkpoint("pause_console")
            assert paused["native"]["console_history"] == 1 and paused["settings"]["paused"]
            assert action("pause")["guard"] == paused["guard"]
            assert change(exposure_ev=1)["guard"] == paused["guard"]
            reject("editor.viewport.set", dict(guard=base["guard"], renderer="raster"))
            reject("editor.viewport.set", dict(guard=paused["guard"], renderer="raster", target_spp=0))
            reject("editor.viewport.set", dict(guard=paused["guard"], seed=1.0))
            reject("editor.viewport.set", dict(renderer="raster"))
            action("reset"); advance("pause_console")
            reset = checkpoint("reset_ipc")
            assert reset["native"]["generation"] > paused["native"]["generation"] and reset["pixels"] == paused["pixels"]
            assert not reset["presentation"]["current"] and reset["presentation"]["has_image"]
            change(target_spp=3); action("resume"); advance("reset_ipc")
            resumed = checkpoint("resume_ipc")
            assert resumed["native"]["generation"] == reset["native"]["generation"]
            advance("resume_ipc")
            restir = checkpoint("restir_gui")
            assert restir["settings"]["direct_lighting"] == "restir_di" and restir["native"]["generation"] > resumed["native"]["generation"]
            pixels = (evidence / "restir.ppm").read_bytes().split(b"\n", 3)[3]
            assert len({pixels[i:i+3] for i in range(0, len(pixels), 3)}) > 4, "Expected visible surface geometry"
            change(medium_mode="scene_media", target_spp=1, initial_candidates=12, temporal_reuse=True,
                   spatial_reuse=True, time_seconds=2, cloud_albedo=.8, cloud_anisotropy=.5)
            advance("restir_gui")
            medium = checkpoint("medium_ipc")
            assert medium["settings"]["time_seconds"] == 2 and medium["settings"]["medium_mode"] == "scene_media"
            source = root / "assets" / "mask.gltf"
            original = source.read_bytes(); source.write_text("invalid glTF fixture")
            scene = client.call("scene.query").value["state"]
            client.call("scene.load", dict(manifest="project.json", guard={k: scene[k] for k in ("document_id", "revision")}))
            reject("editor.viewport.set", dict(guard=medium["guard"], exposure_ev=2))
            advance("medium_ipc")
            failed = checkpoint("failed")
            assert failed["presentation"]["error"] and not failed["presentation"]["current"]
            assert failed["pixels"] == medium["pixels"] and failed["presentation"]["shown_guard"] == medium["presentation"]["shown_guard"]
            source.write_bytes(original)
            scene = client.call("scene.query").value["state"]
            client.call("scene.load", dict(manifest="project.json", guard={k: scene[k] for k in ("document_id", "revision")}))
            advance("failed")
            recovered = checkpoint("recovered")
            assert recovered["presentation"]["current"] and not recovered["presentation"]["error"]
            # Execute the documented Python example with a real guard.
            state = client.call("editor.viewport.query").value
            state = client.call("editor.viewport.set", {
                "guard": state["guard"], "renderer": "pathtrace",
                "direct_lighting": "restir_di", "target_spp": 12,
            }).value
            state = client.call("editor.viewport.pause", {"guard": state["guard"]}).value
            assert state["settings"]["paused"] and state["settings"]["target_spp"] == 12
            report["documented_example"] = state
            action("resume")
            change(target_spp=1000000); advance("recovered")
            checkpoint("active_close")
            client.call("runtime.shutdown")
            assert process.wait(timeout=30) == 0, (directory / "stderr.log").read_text(errors="replace")
            logs = (directory / "stdout.log").read_text(errors="replace")
            assert "validation errors=0" in logs and "liveAllocations=0" in logs
            report["status"] = "passed"
        finally:
            if process is not None and process.poll() is None:
                process.kill(); process.wait(timeout=15)
            (directory / "summary.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
    print("Viewport commands GUI/Console/IPC passed:", directory)


if __name__ == "__main__":
    main()
