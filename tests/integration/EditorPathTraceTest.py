"""M16.9: native GUI inputs, same-queue GPU handoff, IPC and shutdown acceptance."""
import argparse
import json
from pathlib import Path
import shutil
import subprocess
import time
from uuid import uuid4
from decker import Client, guard


def main():
    parser = argparse.ArgumentParser()
    for name in ("editor", "client", "fixtures", "work-dir"):
        parser.add_argument("--" + name, type=Path, required=True)
    args = parser.parse_args()
    directory = args.work_dir / ("pathtrace-" + uuid4().hex)
    root = directory / "project"
    shutil.copytree(args.fixtures, root)
    for asset in (root / "assets").glob("*.gltf"):
        value = json.loads(asset.read_text())
        for material in value["materials"]:
            material["doubleSided"] = True
        asset.write_text(json.dumps(value), encoding="utf-8")
    (root / ".dk-editor-smoke").touch()
    endpoint = "dk-path-" + uuid4().hex
    report = {"status": "failed", "checkpoints": [], "ipc_seconds": []}
    command = [args.editor, "--background", "--root", root, "--pipe", endpoint, "--fixture-camera",
               "--validation", "--pathtrace-smoke", "--screenshot", directory / "workbench.ppm"]
    with (directory / "stdout.log").open("wb") as out, (directory / "stderr.log").open("wb") as err:
        process = subprocess.Popen([str(x) for x in command], stdin=subprocess.DEVNULL, stdout=out, stderr=err,
                                   creationflags=subprocess.CREATE_NO_WINDOW)
        client = Client(endpoint, args.client, timeout=10)
        evidence = root / ".dk-path"

        def checkpoint(name):
            end = time.monotonic() + 90
            while True:
                try:
                    value = json.loads((evidence / (name + ".json")).read_text())
                    break
                except (FileNotFoundError, PermissionError):
                    pass
                if process.poll() is not None or time.monotonic() > end:
                    raise RuntimeError("Editor did not present " + name + ": " + (directory / "stderr.log").read_text())
                time.sleep(.02)
            query = client.call("scene.query").value
            assert query["state"]["document_id"] == value["document_id"]
            assert query["state"]["revision"] == value["revision"]
            report["checkpoints"].append(value)
            return value

        def advance(name):
            (evidence / (name + ".continue")).touch()

        def compare_capture(value):
            name = value["checkpoint"]
            captured = client.capture(dict(guard=dict(document_id=value["document_id"], revision=value["revision"]),
                                          renderer="pathtrace", format="linear_json", output=name + "-capture.json",
                                          width=value["width"], height=value["height"], camera=value["capture_camera"],
                                          pathtrace=value["pathtrace"], validation="required"), timeout=150)
            image = json.loads((root / captured.job["result"]["output"]).read_text())
            gui = json.loads((evidence / (name + ".linear.json")).read_text())
            assert len(gui) == len(image["linear_rgb"]) == value["width"] * value["height"]
            error = max(abs(a-b) for left, right in zip(gui, image["linear_rgb"]) for a, b in zip(left, right))
            assert error <= 1e-5, (name, error)
            report.setdefault("linear_comparisons", []).append(dict(checkpoint=name, max_abs=error, pixels=len(gui)))

        try:
            initial = checkpoint("path")
            assert initial["spp"] == 2 and initial["path_mode"] and initial["undo"] == 0
            compare_capture(initial)
            advance("path")
            paused = checkpoint("paused")
            assert paused["paused"] and paused["target"] == 3 and paused["spp"] == 2 and paused["pixels"] == initial["pixels"]
            advance("paused")
            exposure = checkpoint("exposure")
            assert exposure["generation"] == paused["generation"] and exposure["exposure"] == 2
            assert exposure["pixels"] != paused["pixels"] and exposure["revision"] == initial["revision"]
            advance("exposure")
            reset = checkpoint("reset")
            assert reset["paused"] and reset["spp"] == 0 and reset["generation"] > exposure["generation"]
            advance("reset")
            resumed = checkpoint("resumed")
            assert resumed["pixels"] == exposure["pixels"] and not resumed["paused"]
            compare_capture(resumed)
            advance("resumed")
            camera = checkpoint("camera")
            assert camera["camera"] != resumed["camera"] and camera["generation"] > resumed["generation"]
            assert camera["pixels"] != resumed["pixels"] and camera["revision"] == resumed["revision"]
            compare_capture(camera)
            advance("camera")
            gizmo = checkpoint("gizmo")
            assert gizmo["undo"] == 1 and gizmo["revision"] == camera["revision"] + 1
            assert gizmo["pixels"] != camera["pixels"] and gizmo["generation"] > camera["generation"]
            query = client.call("scene.query").value
            entity = next(e for e in query["entities"] if e["assets"])
            entity["transform"]["translation"][0] += .4
            client.call("entity.set_transform", {"guard": guard(query["state"]), "id": entity["id"], "transform": entity["transform"]})
            advance("gizmo")
            external = checkpoint("external")
            assert external["undo"] == 2 and external["revision"] == gizmo["revision"] + 1
            assert external["generation"] > gizmo["generation"] and external["pixels"] != gizmo["pixels"]
            advance("external")
            resized = checkpoint("resize")
            assert (resized["width"], resized["height"]) != (external["width"], external["height"])
            assert resized["generation"] > external["generation"]
            advance("resize")
            minimized = checkpoint("minimize")
            assert minimized["pixels"] == resized["pixels"] and minimized["spp"] == 2
            advance("minimize")
            raster = checkpoint("raster")
            assert not raster["path_mode"]
            advance("raster")
            returned = checkpoint("returned")
            assert returned["path_mode"] and returned["pixels"] == minimized["pixels"]
            state = client.call("scene.query").value["state"]
            client.call("scene.save", {"guard": guard(state)})
            client.call("scene.load", {"guard": guard(state), "manifest": "project.json"})
            advance("returned")
            reloaded = checkpoint("reloaded")
            assert reloaded["document_id"] != returned["document_id"] and reloaded["undo"] == 0
            assert reloaded["pixels"] == returned["pixels"] and reloaded["generation"] > returned["generation"]
            advance("reloaded")
            closing = checkpoint("closing")
            assert closing["pending"] and closing["spp"] < 64
            for _ in range(12):
                start = time.monotonic()
                client.call("scene.query")
                report["ipc_seconds"].append(time.monotonic() - start)
            assert max(report["ipc_seconds"]) < 3, report["ipc_seconds"]
            advance("closing")
            assert process.wait(timeout=45) == 0, (directory / "stderr.log").read_text()
            log = (directory / "stdout.log").read_text()
            assert "validation errors=0 warnings=0 liveAllocations=0" in log, log
            assert not (directory / "stderr.log").read_text()
            assert (directory / "workbench.ppm").stat().st_size > 1000
            report["status"] = "passed"
        finally:
            if process.poll() is None:
                process.kill()
                process.wait(timeout=10)
            (directory / "report.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
    print("Path tracing GUI/IPC/same queue/background/shutdown passed: " + str(directory))


if __name__ == "__main__":
    main()
