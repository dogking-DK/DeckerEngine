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
    args = parser.parse_args()
    root = args.work_dir / ("path-capture-" + uuid4().hex)
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
            assert names["render.capture"]["effect"] == "external" and not names["render.capture"]["undoable"]
            for name in ("render.capture", "jobs.get", "jobs.wait", "jobs.cancel", "runtime.capabilities"):
                descriptor = call("commands.describe", dict(name=name))
                (root / (name + ".schema.json")).write_text(json.dumps(descriptor, indent=2), encoding="utf-8")
                if name == "render.capture":
                    assert descriptor["parameters"]["properties"]["renderer"]["enum"] == ["raster", "pathtrace"]
            call("scene.load", dict(manifest="project.json"))
            state = call("scene.query")["state"]
            history = call("history.status")
            first = read(submit("first.json"))
            assert call("scene.query")["state"] == state and call("history.status") == history
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
            exposed = read(submit("exposed.json", pathtrace=dict(version=1, target_spp=3, seed=7, max_bounces=4, environment_rgb=[.3, .4, .6], exposure_ev=2)))
            assert exposed["linear_rgb"] == first["linear_rgb"] and exposed["rgba8"] != first["rgba8"]
            full_seed = read(submit("full-seed.json", width=1, height=1,
                                    pathtrace=dict(version=1, target_spp=1, seed=18446744073709551615, environment_rgb=[.3, .4, .6])))
            assert full_seed["provenance"]["pathtrace"]["seed"] == 18446744073709551615
            evidence["checks"].append("exact sample range, repeatability, frozen revision/content, edit changes, PPM and exposure")
            (root / "old.json").write_text("previous", encoding="utf-8")
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
            query = call("environment.query")
            environment = query["environment"]
            environment["enabled"] = True
            call("environment.set", dict(guard=guard(query["state"]), environment=environment))
            finish(submit("old.json"), "failed")
            assert (root / "old.json").read_text() == "previous"
            call("history.undo", dict(guard=guard(call("scene.query")["state"])))
            assert not call("jobs.cancel", dict(id=ppm["job_id"]))["accepted"]
            evidence["checks"].append("observable completed/submitted progress, running cancellation and failed task preserve old file")
            script = subprocess.run([sys.executable, str(args.python_example), "--pipe", endpoint, "--ctl", str(args.client)],
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
            assert python["linear_rgb"] == luau["linear_rgb"] == first["linear_rgb"]
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
