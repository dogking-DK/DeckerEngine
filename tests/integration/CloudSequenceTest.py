"""M15.6 real sequence jobs, frozen inputs, history, scripts and case evidence."""
import argparse
import copy
import json
import shutil
import subprocess
import sys
import time
from pathlib import Path
from uuid import uuid4

from decker import Client, JobError, guard
from decker.render_cases import run_cases, verify_sequence


def main():
    parser = argparse.ArgumentParser()
    for name in ("runner", "client", "fixtures", "work-dir", "python-example", "luau-example"):
        parser.add_argument("--"+name, type=Path, required=True)
    args = parser.parse_args()
    root = args.work_dir / ("cloud-sequence-"+uuid4().hex)
    shutil.copytree(args.fixtures, root)
    for path in (root / "assets").glob("*.gltf"):
        data = json.loads(path.read_text())
        for material in data["materials"]:
            material["doubleSided"] = True
        path.write_text(json.dumps(data), encoding="utf-8")
    endpoint = "dk-cloud-sequence-"+uuid4().hex
    evidence = dict(status="failed", checks=[])
    with (root / "stdout.log").open("wb") as stdout, (root / "stderr.log").open("wb") as stderr:
        process = subprocess.Popen([str(args.runner), "--project-root", str(root), "--pipe", endpoint],
                                   stdin=subprocess.DEVNULL, stdout=stdout, stderr=stderr, creationflags=subprocess.CREATE_NO_WINDOW)
        client = Client(endpoint, args.client, timeout=30)
        def call(method, params=None):
            return client.call(method, params).value
        def edit(environment):
            return call("environment.set", dict(guard=guard(call("scene.query")["state"]), environment=environment))["state"]
        camera = dict(eye=[0, 0, -2], target=[0, 0, 0], up=[0, 1, 0], fov_y=65, near=1, far=10000)
        samples = [dict(camera=camera, time_seconds=t, camera_cut=False) for t in (0, 30)]
        options = dict(mode="reference", cloud_shadows=True, shadow_center_m=[0, 0, 0], shadow_extent_m=8192, shadow_resolution=32)
        def submit(name, sequence=samples, render_options=options, warmup=0):
            return call("render.sequence", dict(guard=guard(call("scene.query")["state"]), output=name,
                        width=16, height=16, samples=sequence, render_options=render_options, warmup_frames=warmup, validation="required"))
        def finish(ticket, sequence=samples):
            job = client.wait_job(ticket["job_id"], timeout=150)
            report = json.loads((root / job["result"]["output"]).read_text())
            verify_sequence(report, ticket, job["result"], sequence)
            return report
        def images(report):
            return [(f["rgba8"], f["linear_rgba"]) for f in report["frames"]]
        try:
            deadline = time.monotonic()+30
            while True:
                try:
                    client.hello(timeout=1)
                    break
                except Exception:
                    assert process.poll() is None and time.monotonic() < deadline
                    time.sleep(.05)
            names = {c["name"]: c for c in call("commands.list")}
            for name in ("render.capture", "render.sequence"):
                assert names[name]["effect"] == "external" and not names[name]["undoable"]
                descriptor = call("commands.describe", {"name": name})
                assert "render_options" in descriptor["parameters"]["properties"]
                (root / (name+".schema.json")).write_text(json.dumps(descriptor, indent=2))
            assert names["environment.preset"]["effect"] == "query"
            call("scene.load", {"manifest": "project.json"})
            environment = call("environment.preset", {"name": "sparse"})["environment"]
            environment["atmosphere"]["planet_center_m"][1] -= 500
            environment["sun"]["direction"] = [.2, .8, -.3]
            environment["clouds"].update(weather_resolution=16, base_resolution=16, detail_resolution=8,
                                          coverage=.85, extinction_per_m=.0003)
            state = edit(environment)
            history = call("history.status")
            first = submit("reference.json")
            cleared = copy.deepcopy(environment)
            cleared["clouds"]["coverage"] = 0
            edit(cleared)
            reference = finish(first)
            assert first["revision"] == state["revision"] and reference["provenance"]["environment"]["clouds"]["coverage"] == .85
            assert all(f["shadow_generation"] > 0 and not f["history"]["available"] for f in reference["frames"])
            edit(environment)
            assert images(finish(submit("reference-repeat.json"))) == images(reference)
            evidence["checks"].append("frozen reference, replay, shadow applied, no history")
            capture = client.capture(dict(guard=guard(call("scene.query")["state"]), output="time30.ppm", width=16, height=16,
                                          camera=camera, time_seconds=30, render_options=options, validation="required"), timeout=150)
            rgba = reference["frames"][1]["rgba8"]
            expected = b"P6\n16 16\n255\n"+bytes(v for i, v in enumerate(rgba) if i % 4 != 3)
            assert (root / capture.job["result"]["output"]).read_bytes() == expected
            temporal_samples = [dict(camera=camera, time_seconds=t, camera_cut=cut) for t, cut in ((0, False), (.03, False), (0, False), (40, False), (40, True))]
            temporal_options = dict(options, mode="temporal")
            temporal = finish(submit("temporal.json", temporal_samples, temporal_options, 2), temporal_samples)
            resets = [f["history"]["reset"] for f in temporal["frames"]]
            assert temporal["frames"][0]["history"]["available"] and temporal["frames"][1]["history"]["available"]
            assert resets[2:4] == ["time", "time"] and resets[4] == "camera", resets
            assert images(finish(submit("temporal-repeat.json", temporal_samples, temporal_options, 2), temporal_samples)) == images(temporal)
            evidence["checks"].append("explicit time, isolated temporal warmup, rewind/jump/cut and deterministic replay")
            (root / "old.json").write_text("previous")
            failed = submit("old.json", render_options=dict(options, shadow_extent_m=16, shadow_center_m=[100000, 0, 0]))
            try:
                finish(failed)
                raise AssertionError("Invalid coverage succeeded")
            except JobError as error:
                assert error.job["state"] == "failed" and error.job["result"] is None
            assert (root / "old.json").read_text() == "previous"
            cancelled = submit("old.json", render_options=temporal_options, warmup=64)
            assert call("jobs.cancel", {"id": cancelled["job_id"]})["accepted"]
            try:
                finish(cancelled)
                raise AssertionError("Cancelled sequence succeeded")
            except JobError as error:
                assert error.job["state"] == "cancelled"
            assert (root / "old.json").read_text() == "previous"
            evidence["checks"].append("failure and cancellation preserve the previous atomic report")
            case = dict(id="reference", environment=environment, width=16, height=16, exposure=1,
                        render_options=options, warmup_frames=0, samples=samples)
            candidate = copy.deepcopy(case)
            candidate.update(id="temporal", render_options=temporal_options, warmup_frames=32)
            manifest = dict(schema_version=1, project="project.json", cases=[case, candidate],
                            comparisons=[dict(reference="reference", candidate="temporal")])
            report = run_cases(client, root, manifest, "cases", args.runner)
            assert report["status"] == "passed", report["comparisons"]
            broken = copy.deepcopy(case)
            broken["id"] = "invalid"
            broken["environment"]["clouds"]["density"] = -1
            manifest["cases"] = [case, broken]
            manifest["comparisons"] = [dict(reference="reference", candidate="invalid")]
            report = run_cases(client, root, manifest, "failed-cases", args.runner)
            assert report["status"] == "failed" and report["cases"][1]["artifacts"] == [] and report["cases"][1]["wall_ms"] is None
            assert report["cases"][1]["error"] and report["comparisons"][0]["metrics"] is None
            evidence["checks"].append("versioned cases, source hashes, linear comparison and explicit failed/missing evidence")
            script = subprocess.run([sys.executable, str(args.python_example), "--pipe", endpoint, "--ctl", str(args.client)],
                                    capture_output=True, text=True, timeout=150, creationflags=subprocess.CREATE_NO_WINDOW)
            assert script.returncode == 0, (script.stdout, script.stderr)
            python_report = json.loads((root / "cloud-python.json").read_text())
            call("runtime.shutdown")
            assert process.wait(timeout=30) == 0
            assert not (root / "stderr.log").read_bytes()
            script = subprocess.run([str(args.runner), "--project-root", str(root), "--script", str(args.luau_example), "--script-timeout-ms", "150000"],
                                    capture_output=True, text=True, timeout=160, creationflags=subprocess.CREATE_NO_WINDOW)
            assert script.returncode == 0 and not script.stderr, (script.stdout, script.stderr)
            assert images(python_report) == images(json.loads((root / "cloud-luau.json").read_text()))
            evidence["checks"].append("real Python and Luau examples produce identical images and linear samples")
            evidence["status"] = "passed"
        finally:
            if process.poll() is None:
                process.kill()
                process.wait(timeout=10)
            (root / "evidence.json").write_text(json.dumps(evidence, indent=2), encoding="utf-8")
    print("Cloud sequence/cases/scripts passed: "+str(root))


if __name__ == "__main__":
    main()
