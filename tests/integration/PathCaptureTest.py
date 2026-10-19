"""M16.10: actual command registry, fixed samples, frozen inputs, progress and atomic output."""
import argparse
import copy
import json
from pathlib import Path
import shutil
import subprocess
import sys
import time
from uuid import uuid4
from decker import Client, guard


def main():
    parser = argparse.ArgumentParser()
    for name in ("runner", "client", "fixtures", "work-dir", "python-example", "luau-example"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--media", action="store_true")
    args = parser.parse_args()
    root = args.work_dir / (("media-capture-" if args.media else "path-capture-") + uuid4().hex)
    shutil.copytree(args.fixtures, root)
    for asset in (root / "assets").glob("*.gltf"):
        data = json.loads(asset.read_text())
        for material in data["materials"]:
            material["doubleSided"] = True
        asset.write_text(json.dumps(data), encoding="utf-8")
    endpoint = "dk-path-capture-" + uuid4().hex
    evidence = {"status": "failed", "progress": [], "checks": []}
    with (root / "stdout.log").open("wb") as out, (root / "stderr.log").open("wb") as err:
        process = subprocess.Popen([str(args.runner), "--project-root", str(root), "--pipe", endpoint],
                                   stdin=subprocess.DEVNULL, stdout=out, stderr=err, creationflags=subprocess.CREATE_NO_WINDOW)
        client = Client(endpoint, args.client, timeout=30)
        def call(method, params=None):
            return client.call(method, params).value
        def submit(name, spp=3, **extra):
            params = dict(guard=guard(call("scene.query")["state"]), renderer="pathtrace", format="linear_json", output=name,
                          width=48, height=32, validation="required",
                          camera=dict(eye=[0, 0, -2], target=[0, 0, 0], near=.05, far=10000),
                          pathtrace=dict(version=1, target_spp=spp, seed=7, max_bounces=4, environment_rgb=[.3, .4, .6]))
            if args.media:
                params["pathtrace"].update(version=2, medium_mode="scene_media")
            params.update(extra)
            return call("render.capture", params)
        def finish(ticket, state="succeeded"):
            deadline = time.monotonic() + 150
            previous = 0
            while True:
                job = call("jobs.wait", dict(id=ticket["job_id"], timeout_ms=100))["job"]
                p = job["progress"]
                assert p["unit"] == "spp" and previous <= p["completed"] <= p["submitted"] <= p["total"]
                previous = p["completed"]
                if job["state"] in ("succeeded", "failed", "cancelled"):
                    assert job["state"] == state, job
                    if state != "succeeded":
                        assert job["result"] is None
                    return job
                assert time.monotonic() < deadline
        def read(ticket):
            job = finish(ticket)
            report = json.loads((root / ticket["output"]).read_text())
            for key in ("document_id", "scene_id", "revision", "frame", "width", "height"):
                assert report[key] == ticket[key] == job["result"][key]
            assert report["provenance"] == job["result"]["provenance"]
            assert report["sample_first"] == 0 and report["sample_count"] == job["progress"]["completed"]
            assert report["provenance"]["actual_spp"] == report["sample_count"]
            assert report["provenance"]["input_fingerprint_version"] == 3
            assert report["gpu_ms"] is None and report["wall_ms"] > 0 and report["device"]["name"]
            assert report["memory"]["peak_bytes"] <= report["memory"]["limit_bytes"] == 1073741824
            return report
        try:
            deadline = time.monotonic() + 30
            while True:
                try:
                    client.hello(timeout=1)
                    break
                except Exception:
                    assert process.poll() is None and time.monotonic() < deadline
                    time.sleep(.05)
            assert call("runtime.capabilities")["pathtrace_capture"]
            names = {x["name"]: x for x in call("commands.list")}
            for name in ("render.capture", "render.sequence"):
                assert names[name]["effect"] == "external" and not names[name]["undoable"]
            for name in ("render.capture", "render.sequence", "jobs.get", "jobs.wait", "jobs.cancel", "runtime.capabilities"):
                descriptor = call("commands.describe", dict(name=name))
                (root / (name + ".schema.json")).write_text(json.dumps(descriptor, indent=2), encoding="utf-8")
                if name in ("render.capture", "render.sequence"):
                    fields = descriptor["parameters"]["properties"]
                    lens = fields["camera"] if name == "render.capture" else fields["samples"]["items"]["properties"]["camera"]
                    assert lens["properties"]["projection"]["enum"] == ["perspective", "orthographic"] and "ortho_height" in lens["properties"]
                    result_lens = descriptor["result"]["properties"]["provenance"]["properties"]["camera"]
                    assert "projection" in result_lens["required"] and "fov_y" not in result_lens["required"]
                    assert result_lens["properties"]["primary_ray_origin"]["enum"] == ["near_plane"]
                if name == "render.capture":
                    assert descriptor["parameters"]["properties"]["renderer"]["enum"] == ["raster", "pathtrace"]
                    fields = descriptor["parameters"]["properties"]["pathtrace"]["properties"]
                    assert fields["version"]["maximum"] == 2 and fields["medium_mode"]["enum"] == ["surface_only", "scene_media"]
                    assert fields["max_tracking_steps"]["maximum"] == 268435456
                    fingerprint = descriptor["result"]["properties"]["provenance"]["properties"]["input_fingerprint_version"]
                    assert fingerprint["minimum"] == fingerprint["maximum"] == 3
            call("scene.load", dict(manifest="project.json"))
            if args.media:
                env = call("environment.query")["environment"]
                env["enabled"] = True
                env["atmosphere"]["planet_center_m"][1] -= 10
                env["clouds"].update(enabled=True, coverage=1, extinction_per_m=.00005,
                                     weather_resolution=16, base_resolution=16, detail_resolution=16,
                                     wind_velocity_mps=[1000, 0])
                call("environment.set", dict(guard=guard(call("scene.query")["state"]), environment=env))
                call("scene.save", dict(guard=guard(call("scene.query")["state"])))
            state = call("scene.query")["state"]
            history = call("history.status")
            first = read(submit("first.json"))
            assert call("scene.query")["state"] == state and call("history.status") == history
            ortho_camera = dict(eye=[0, 0, -2], target=[0, 0, 0], projection="orthographic", ortho_height=3, near=.05, far=10000)
            if args.media:
                ortho_camera.update(eye=[0, 2, -2], target=[0, 2, 0])
            orthographic = read(submit("orthographic.json", camera=ortho_camera))
            effective = orthographic["provenance"]["camera"]
            assert effective == dict(ortho_camera, up=[0, 1, 0], primary_ray_origin="near_plane")
            assert first["provenance"]["camera"]["projection"] == "perspective" and first["provenance"]["camera"]["fov_y"] == 65
            assert orthographic["linear_rgb"] != first["linear_rgb"]
            (root / "lens-old.json").write_text("previous")
            for camera in (dict(ortho_camera, fov_y=65), dict(ortho_camera, ortho_height=0), dict(ortho_camera, projection="perspective"),
                           {k:v for k,v in ortho_camera.items() if k != "ortho_height"}):
                try:
                    submit("lens-old.json", camera=camera)
                except Exception:
                    pass
                else:
                    raise AssertionError("Invalid lens was accepted")
                assert (root / "lens-old.json").read_text() == "previous"
            assert call("scene.query")["state"] == state and call("history.status") == history
            if not args.media:
                samples = [dict(camera=ortho_camera, time_seconds=0), dict(camera=dict(ortho_camera, ortho_height=4), time_seconds=1)]
                ticket = call("render.sequence", dict(guard=guard(state), output="ortho-sequence.json", width=16, height=16, samples=samples, validation="required"))
                while True:
                    job = call("jobs.wait", dict(id=ticket["job_id"], timeout_ms=1000))["job"]
                    if job["state"] in ("succeeded", "failed", "cancelled"): break
                assert job["state"] == "succeeded", job
                sequence = json.loads((root / "ortho-sequence.json").read_text())
                assert [frame["camera"]["ortho_height"] for frame in sequence["frames"]] == [3, 4]
                assert all(frame["camera"]["primary_ray_origin"] == "near_plane" for frame in sequence["frames"])
            evidence["checks"].append("perspective compatibility, orthographic provenance, invalid lenses and raster sequence")
            defaults = read(submit("defaults.json", width=1, height=1, camera=dict(eye=[0, 0, -2], target=[0, 0, 0])))
            assert defaults["provenance"]["camera"] == dict(eye=[0, 0, -2], target=[0, 0, 0], up=[0, 1, 0], projection="perspective", fov_y=65, near=.05, far=100)
            frozen = submit("frozen.json")
            assert frozen["provenance"]["actual_spp"] == 0 and frozen["provenance"]["input_fingerprint"] is None
            query = call("scene.query")
            entity = next(e for e in query["entities"] if e["assets"])
            transform = copy.deepcopy(entity["transform"])
            transform["translation"][0] += .75
            call("entity.set_transform", dict(guard=guard(query["state"]), id=entity["id"], transform=transform))
            same = read(frozen)
            assert same["linear_rgb"] == first["linear_rgb"] and same["rgba8"] == first["rgba8"]
            assert same["provenance"]["input_fingerprint"] == first["provenance"]["input_fingerprint"]
            edited = read(submit("edited.json"))
            assert edited["linear_rgb"] != first["linear_rgb"] and edited["provenance"]["input_fingerprint"] != first["provenance"]["input_fingerprint"]
            call("history.undo", dict(guard=guard(call("scene.query")["state"])))
            ppm = submit("display.ppm", format="ppm")
            finish(ppm)
            rgba = first["rgba8"]
            assert (root / "display.ppm").read_bytes() == b"P6\n48 32\n255\n" + bytes(v for i, v in enumerate(rgba) if i % 4 != 3)
            path = dict(version=2, medium_mode="scene_media", target_spp=3, seed=7, max_bounces=4, environment_rgb=[.3, .4, .6]) if args.media else dict(version=1, target_spp=3, seed=7, max_bounces=4, environment_rgb=[.3, .4, .6])
            exposed = read(submit("exposed.json", pathtrace=dict(path, exposure_ev=2)))
            assert exposed["linear_rgb"] == first["linear_rgb"] and exposed["rgba8"] != first["rgba8"]
            full_seed = read(submit("full-seed.json", width=1, height=1,
                                    pathtrace=dict(path, target_spp=1, seed=18446744073709551615)))
            assert full_seed["provenance"]["pathtrace"]["seed"] == 18446744073709551615
            evidence["checks"].append("exact sample range, repeatability, frozen revision/content, edit changes, PPM and exposure")
            (root / "old.json").write_text("previous", encoding="utf-8")
            if args.media:
                provenance = first["provenance"]
                assert provenance["model_version"] == 4 and provenance["sampler_version"] == 2
                assert provenance["cloud_field_hash"] not in (None, "fnv1a64:0000000000000000")
                shifted = read(submit("time.json", pathtrace=dict(path, time_seconds=2)))
                assert shifted["linear_rgb"] != first["linear_rgb"]
                assert shifted["provenance"]["geometry_fingerprint"] == provenance["geometry_fingerprint"]
                assert shifted["provenance"]["cloud_field_hash"] == provenance["cloud_field_hash"]
                surface = dict(path, medium_mode="surface_only")
                surface_image = read(submit("surface.json", pathtrace=surface))
                legacy = dict(surface, version=1, surface_only=True); del legacy["medium_mode"]
                old = read(submit("legacy.json", pathtrace=legacy))
                assert old["linear_rgb"] == surface_image["linear_rgb"] != first["linear_rgb"]
                for i in range(2):
                    bad = copy.deepcopy(env); bad["clouds"].update(shape_scale_m=1, seed=10+i)
                    call("environment.set", dict(guard=guard(call("scene.query")["state"]), environment=bad))
                    finish(submit("old.json"), "failed")
                    assert (root / "old.json").read_text() == "previous"
                call("environment.set", dict(guard=guard(call("scene.query")["state"]), environment=env))
                recovered = read(submit("recovered.json"))
                assert recovered["linear_rgb"] == first["linear_rgb"]
                assert recovered["provenance"]["cloud_field_hash"] == provenance["cloud_field_hash"]
                evidence["checks"].append("v2 media provenance, fixed wind time, surface/v1 equality and repeated invalid-field recovery")
            long = submit("old.json", spp=4096, width=64, height=64)
            deadline = time.monotonic() + 90
            while True:
                job = call("jobs.wait", dict(id=long["job_id"], timeout_ms=20))["job"]
                evidence["progress"].append(job["progress"])
                assert job["state"] in ("queued", "running"), job
                assert time.monotonic() < deadline
                if job["progress"]["completed"] >= 1:
                    break
            (root / "queued.json").write_text("previous", encoding="utf-8")
            queued = submit("queued.json")
            stopped = call("jobs.cancel", dict(id=queued["job_id"]))
            assert stopped["accepted"] and stopped["job"]["state"] == "cancelled" and stopped["job"]["progress"]["completed"] == 0
            assert (root / "queued.json").read_text() == "previous"
            assert call("jobs.cancel", dict(id=long["job_id"]))["accepted"]
            finish(long, "cancelled")
            assert (root / "old.json").read_text() == "previous"
            if args.media:
                finish(submit("old.json", pathtrace=dict(path, max_tracking_steps=1, max_steps_per_path=1)), "failed")
            else:
                query = call("environment.query")
                environment = query["environment"]
                environment["enabled"] = True
                call("environment.set", dict(guard=guard(query["state"]), environment=environment))
                finish(submit("old.json"), "failed")
                call("history.undo", dict(guard=guard(call("scene.query")["state"])))
            assert (root / "old.json").read_text() == "previous"
            assert not call("jobs.cancel", dict(id=ppm["job_id"]))["accepted"]
            evidence["checks"].append("observable completed/submitted progress, running cancellation and failed task preserve old file")
            script = subprocess.run([sys.executable, str(args.python_example), "--pipe", endpoint, "--ctl", str(args.client), "--orthographic"] + (["--media"] if args.media else []),
                                    capture_output=True, text=True, timeout=160, creationflags=subprocess.CREATE_NO_WINDOW)
            assert script.returncode == 0, (script.stdout, script.stderr)
            python = json.loads((root / "path-python.json").read_text())
            (root / "shutdown.json").write_text("previous", encoding="utf-8")
            closing = submit("shutdown.json", spp=4096, width=64, height=64)
            deadline = time.monotonic() + 90
            while call("jobs.wait", dict(id=closing["job_id"], timeout_ms=20))["job"]["progress"]["completed"] < 1:
                assert time.monotonic() < deadline
            call("runtime.shutdown")
            assert process.wait(timeout=30) == 0 and not (root / "stderr.log").read_bytes()
            assert (root / "shutdown.json").read_text() == "previous"
            script = subprocess.run([str(args.runner), "--project-root", str(root), "--script", str(args.luau_example), "--script-timeout-ms", "150000"],
                                    capture_output=True, text=True, timeout=160, creationflags=subprocess.CREATE_NO_WINDOW)
            assert script.returncode == 0 and not script.stderr, (script.stdout, script.stderr)
            luau = json.loads((root / "path-luau.json").read_text())
            assert python["linear_rgb"] == luau["linear_rgb"] == orthographic["linear_rgb"]
            assert python["rgba8"] == luau["rgba8"]
            evidence["checks"].append("real Python and Luau examples match linear pixels")
            evidence["status"] = "passed"
        finally:
            if process.poll() is None:
                process.kill()
                process.wait(timeout=10)
            (root / "evidence.json").write_text(json.dumps(evidence, indent=2), encoding="utf-8")
    print("Path capture/progress/scripts passed: " + str(root))


if __name__ == "__main__":
    main()
