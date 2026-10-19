"""M16.9: native GUI inputs, same-queue GPU handoff, IPC and shutdown acceptance."""
import argparse
import copy
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
    parser.add_argument("--media", action="store_true")
    args = parser.parse_args()
    directory = args.work_dir / (("media-pathtrace-" if args.media else "pathtrace-") + uuid4().hex)
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
               "--validation", "--media-pathtrace-smoke" if args.media else "--pathtrace-smoke", "--screenshot", directory / "workbench.ppm"]
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
            if args.media:
                source = image["provenance"]
                assert source["model_version"] == value["model_version"] and source["sampler_version"] == value["sampler_version"]
                assert source["geometry_fingerprint"] == value["geometry_fingerprint"]
                assert source["input_fingerprint"] == value["input_fingerprint"]
                if value["mode"] == "scene_media":
                    assert source["cloud_field_hash"] == value["field_hash"] and source["time_seconds"] == value["time_seconds"]
            report.setdefault("linear_comparisons", []).append(dict(checkpoint=name, max_abs=error, pixels=len(gui)))

        def compare_projections(previous):
            for name in ("orthographic", "ortho_height", "perspective"):
                value = checkpoint(name)
                camera = value["capture_camera"]
                assert camera["projection"] == ("perspective" if name == "perspective" else "orthographic")
                assert value["generation"] > previous["generation"] and value["revision"] == previous["revision"]
                if name == "ortho_height":
                    assert camera["ortho_height"] == 3
                if args.media:
                    assert value["geometry_fingerprint"] == previous["geometry_fingerprint"]
                compare_capture(value); advance(name)
                previous = value

        def run_media():
            def call(method, params=None):
                return client.call(method, params).value
            def set_environment(env):
                call("environment.set", dict(guard=guard(call("scene.query")["state"]), environment=env))
            first = checkpoint("surface")
            assert first["mode"] == "surface_only" and first["current"]
            compare_capture(first); advance("surface")
            air = checkpoint("air")
            assert air["mode"] == "scene_media" and air["generation"] > first["generation"]
            assert air["pixels"] != first["pixels"] and air["field_hash"] == "fnv1a64:0000000000000000"
            compare_capture(air)
            env = call("environment.query")["environment"]
            assert env["enabled"]  # enabled through the actual Environment panel
            env["atmosphere"]["planet_center_m"][1] -= 10
            env["clouds"].update(enabled=True, coverage=1, extinction_per_m=.00005,
                                 weather_resolution=16, base_resolution=16, detail_resolution=16, wind_velocity_mps=[1000, 0])
            set_environment(env); advance("air")
            cloud = checkpoint("cloud")
            assert cloud["field_hash"] != air["field_hash"] and cloud["geometry_fingerprint"] == air["geometry_fingerprint"]
            assert cloud["generation"] > air["generation"] and cloud["pixels"] != air["pixels"]
            compare_capture(cloud); advance("cloud")
            shifted = checkpoint("time")
            assert shifted["time_seconds"] == 2 and shifted["generation"] > cloud["generation"]
            assert shifted["field_hash"] == cloud["field_hash"] and shifted["pixels"] != cloud["pixels"]
            compare_capture(shifted); advance("time")
            exposure = checkpoint("exposure")
            assert exposure["exposure"] == 2 and exposure["generation"] == shifted["generation"]
            assert exposure["pixels"] != shifted["pixels"]
            assert (evidence / "exposure.linear.json").read_bytes() == (evidence / "time.linear.json").read_bytes()
            bad = copy.deepcopy(env); bad["clouds"].update(shape_scale_m=1, seed=11)
            set_environment(bad); advance("exposure")
            for name, previous in (("rejected", "exposure"), ("rejected_again", "rejected")):
                failed = checkpoint(name)
                assert failed["error"] and not failed["current"] and failed["shown_revision"] == exposure["shown_revision"]
                assert failed["pixels"] == exposure["pixels"] and failed["field_hash"] == exposure["field_hash"]
                if name == "rejected":
                    bad["clouds"]["seed"] = 12; set_environment(bad)
                else:
                    set_environment(env)
                advance(name)
            recovered = checkpoint("recovered")
            assert recovered["current"] and recovered["pixels"] == exposure["pixels"] and recovered["field_hash"] == exposure["field_hash"]
            compare_capture(recovered); advance("recovered")
            surface = checkpoint("surface_again")
            assert surface["mode"] == "surface_only" and surface["generation"] > recovered["generation"]
            compare_capture(surface); advance("surface_again")
            media = checkpoint("media_again")
            assert media["mode"] == "scene_media" and media["generation"] > surface["generation"] and media["pixels"] == recovered["pixels"]
            state = call("scene.query")["state"]
            call("scene.save", dict(guard=guard(state)))
            call("scene.load", dict(guard=guard(state), manifest="project.json")); advance("media_again")
            reloaded = checkpoint("reloaded")
            assert reloaded["document_id"] != media["document_id"] and reloaded["generation"] > media["generation"]
            assert reloaded["pixels"] == media["pixels"]
            compare_capture(reloaded)
            command = dict(method="render.capture", params=dict(guard=guard(call("scene.query")["state"]),
                           renderer="pathtrace", format="linear_json", output="console-capture.json",
                           width=reloaded["width"], height=reloaded["height"], camera=reloaded["capture_camera"],
                           pathtrace=reloaded["pathtrace"], validation="required"))
            (evidence / "command.json").write_text(json.dumps(command), encoding="utf-8"); advance("reloaded")
            console = checkpoint("console")
            assert console["generation"] > reloaded["generation"] and console["pixels"] == reloaded["pixels"]
            end = time.monotonic() + 90
            while not (root / "console-capture.json").is_file():
                assert process.poll() is None and time.monotonic() < end, (directory / "stderr.log").read_text()
                time.sleep(.02)
            image = json.loads((root / "console-capture.json").read_text())
            linear = json.loads((evidence / "console.linear.json").read_text())
            error = max(abs(a-b) for left, right in zip(linear, image["linear_rgb"]) for a,b in zip(left,right))
            assert error <= 1e-5 and image["provenance"]["cloud_field_hash"] == console["field_hash"]
            report["console_capture_max_abs"] = error; advance("console")
            compare_projections(console)
            closing = checkpoint("closing")
            assert closing["pending"] and closing["spp"] < 64
            for _ in range(6):
                start = time.monotonic(); call("scene.query"); report["ipc_seconds"].append(time.monotonic()-start)
            assert max(report["ipc_seconds"]) < 3
            advance("closing")

        try:
            if args.media:
                run_media()
            else:
                initial = checkpoint("path")
                assert initial["spp"] == 2 and initial["path_mode"] and initial["undo"] == 0
                assert initial["capture_camera"]["fov_y"] == 55
                compare_capture(initial)
                advance("path")
                paused = checkpoint("paused")
                assert paused["paused"] and paused["target"] == 3 and paused["spp"] == 2 and paused["pixels"] == initial["pixels"]
                assert paused["generation"] == initial["generation"] and paused["camera"] == initial["camera"]
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
                assert camera["navigation_updates"] >= 2
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
                compare_projections(reloaded)
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
