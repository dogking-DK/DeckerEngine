"""M19.7: fresh full-view epochs, effective provenance, scripts and atomic failure."""
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
    parser.add_argument("--skip-luau", action="store_true", help="Runner was built without Luau")
    args = parser.parse_args()
    root = args.work_dir / ("restir-capture-" + uuid4().hex)
    shutil.copytree(args.fixtures, root)
    for asset in (root / "assets").glob("*.gltf"):
        value = json.loads(asset.read_text())
        for material in value["materials"]:
            material["doubleSided"] = True
        asset.write_text(json.dumps(value), encoding="utf-8")
    endpoint = "dk-restir-" + uuid4().hex
    evidence = dict(status="failed", cases=[])
    with (root / "stdout.log").open("wb") as out, (root / "stderr.log").open("wb") as err:
        process = subprocess.Popen([str(args.runner), "--project-root", str(root), "--pipe", endpoint],
                                   stdin=subprocess.DEVNULL, stdout=out, stderr=err, creationflags=subprocess.CREATE_NO_WINDOW)
        client = Client(endpoint, args.client, timeout=30)
        def call(method, params=None):
            return client.call(method, params).value
        def submit(name, path, **extra):
            params = dict(guard=guard(call("scene.query")["state"]), renderer="pathtrace", format="linear_json", output=name,
                          width=24, height=16, validation="required", pathtrace=path,
                          camera=dict(eye=[0, 0, -2], target=[0, 0, 0], near=.05, far=10000))
            params.update(extra)
            return call("render.capture", params)
        def finish(ticket, expected="succeeded"):
            deadline, previous = time.monotonic() + 150, 0
            while True:
                job = call("jobs.wait", dict(id=ticket["job_id"], timeout_ms=100))["job"]
                p = job["progress"]
                assert p["unit"] == "spp" and previous <= p["completed"] <= p["submitted"] <= p["total"], p
                previous = p["completed"]
                if job["state"] in ("succeeded", "failed", "cancelled"):
                    assert job["state"] == expected, job
                    return job
                assert time.monotonic() < deadline
        def read(ticket):
            job = finish(ticket)
            value = json.loads((root / ticket["output"]).read_text())
            source = value["provenance"]
            assert source == job["result"]["provenance"] and source["actual_spp"] == value["sample_count"] == 3
            assert value["sample_first"] == 0 and value["validation"]
            assert source["input_fingerprint_version"] == 3
            if source["pathtrace"]["version"] == 3:
                assert value["schema_version"] == 2 and source["provenance_version"] == 1
                assert source["history"] == dict(start="empty", sample_first=0, epoch_first=0, temporal_history_epoch=None)
                assert ticket["provenance"]["reuse"] is None
                if source["pathtrace"]["direct_lighting"] == "restir_di":
                    assert source["rtxdi_version"] == "3.1.0" and source["restir_sampler_version"] == 1
                    assert source["reuse"] is not None
                    assert source["pathtrace"]["restir"]["version"] == 1
            evidence["cases"].append(dict(output=ticket["output"], provenance=source))
            return value
        def equal(a, b):
            assert a["linear_rgb"] == b["linear_rgb"] and a["rgba8"] == b["rgba8"]
            assert a["provenance"]["input_fingerprint"] == b["provenance"]["input_fingerprint"]
        try:
            deadline = time.monotonic() + 30
            while True:
                try:
                    client.hello(timeout=1)
                    break
                except Exception:
                    assert process.poll() is None and time.monotonic() < deadline
                    time.sleep(.05)
            call("scene.load", dict(manifest="project.json"))
            descriptor = call("commands.describe", dict(name="render.capture"))
            (root / "capture.schema.json").write_text(json.dumps(descriptor, indent=2))
            fields = descriptor["parameters"]["properties"]["pathtrace"]["properties"]
            assert fields["version"]["maximum"] == 3 and fields["direct_lighting"]["enum"] == ["classic_mis", "restir_di"]
            assert "pathtrace" not in call("commands.describe", dict(name="render.sequence"))["parameters"]["properties"]
            for media in (False, True):
                if media:
                    env = call("environment.query")["environment"]
                    env["enabled"] = True
                    env["atmosphere"]["planet_center_m"][1] -= 10
                    env["clouds"].update(enabled=True, coverage=1, extinction_per_m=.00005, weather_resolution=16,
                                         base_resolution=16, detail_resolution=16, wind_velocity_mps=[1000, 0])
                    call("environment.set", dict(guard=guard(call("scene.query")["state"]), environment=env))
                    call("scene.save", dict(guard=guard(call("scene.query")["state"])))
                state, history = call("scene.query")["state"], call("history.status")
                base = dict(version=3, medium_mode="scene_media" if media else "surface_only", direct_lighting="restir_di",
                            target_spp=3, seed=7, max_bounces=4, environment_rgb=[.3, .4, .6])
                classic = dict(base, direct_lighting="classic_mis")
                legacy = dict(classic, version=2); del legacy["direct_lighting"]
                equal(read(submit(f"{media}-classic.json", classic)), read(submit(f"{media}-v2.json", legacy)))
                for mode in ("initial", "temporal", "spatial", "joint"):
                    config = dict(initial_candidates=8)
                    if mode in ("temporal", "joint"): config["temporal"] = {}
                    if mode in ("spatial", "joint"): config["spatial"] = {}
                    path = dict(base, restir=config)
                    first = read(submit(f"{media}-{mode}.json", path))
                    equal(first, read(submit(f"{media}-{mode}-again.json", path)))
                    reuse = first["provenance"]["reuse"]
                    if "temporal" in config: assert reuse["temporal"]["reused"] > 0 and reuse["temporal"]["max_age"] > 0
                    else: assert reuse["temporal"]["reused"] == 0
                    if "spatial" in config: assert reuse["spatial"]["reused"] > 0
                    else: assert reuse["spatial"]["reused"] == 0
                    assert call("scene.query")["state"] == state and call("history.status") == history
                camera = dict(eye=[0, 2, -2] if media else [0, 0, -2], target=[0, 2, 0] if media else [0, 0, 0],
                              projection="orthographic", ortho_height=3, near=.05, far=10000)
                orthographic = read(submit(f"{media}-ortho.json", path, camera=camera, width=48, height=32))
                exposed = read(submit(f"{media}-exposed.json", dict(path, exposure_ev=2), camera=camera, width=48, height=32))
                assert exposed["linear_rgb"] == orthographic["linear_rgb"] and exposed["rgba8"] != orthographic["rgba8"]
                (root / "old.json").write_text("previous")
                finish(submit("old.json", dict(path, restir=dict(config, max_candidate_evaluations=1))), "failed")
                assert (root / "old.json").read_text() == "previous"
                # A running producer and a queued capture have independent history and cancellation.
                long = submit("old.json", dict(path, target_spp=4096), width=64, height=64)
                deadline = time.monotonic() + 90
                while call("jobs.wait", dict(id=long["job_id"], timeout_ms=20))["job"]["progress"]["completed"] < 1:
                    assert time.monotonic() < deadline
                queued = submit("old.json", path)
                assert call("jobs.cancel", dict(id=queued["job_id"]))["accepted"]
                assert call("jobs.cancel", dict(id=long["job_id"]))["accepted"]
                finish(queued, "cancelled"); finish(long, "cancelled")
                assert (root / "old.json").read_text() == "previous"
            script = subprocess.run([sys.executable, str(args.python_example), "--pipe", endpoint, "--ctl", str(args.client),
                                     "--media", "--orthographic", "--restir", "joint"], capture_output=True, text=True,
                                    timeout=160, creationflags=subprocess.CREATE_NO_WINDOW)
            assert script.returncode == 0, (script.stdout, script.stderr)
            equal(json.loads((root / "path-python.json").read_text()), orthographic)
            closing = submit("old.json", dict(path, target_spp=4096), width=64, height=64)
            call("runtime.shutdown")
            assert process.wait(timeout=30) == 0 and not (root / "stderr.log").read_bytes()
            assert (root / "old.json").read_text() == "previous"
            if not args.skip_luau:
                script = subprocess.run([str(args.runner), "--project-root", str(root), "--script", str(args.luau_example),
                                         "--script-timeout-ms", "150000"], capture_output=True, text=True, timeout=160,
                                        creationflags=subprocess.CREATE_NO_WINDOW)
                assert script.returncode == 0 and not script.stderr, (script.stdout, script.stderr)
                equal(json.loads((root / "path-luau.json").read_text()), orthographic)
            else:
                evidence["luau"] = "not built; example not run"
            evidence["status"] = "passed"
        finally:
            if process.poll() is None:
                process.kill(); process.wait(timeout=10)
            (root / "evidence.json").write_text(json.dumps(evidence, indent=2), encoding="utf-8")
    print("ReSTIR capture passed: " + str(root))


if __name__ == "__main__":
    main()
