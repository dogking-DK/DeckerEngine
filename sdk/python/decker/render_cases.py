"""Versioned cloud render cases, verified artifacts and linear comparisons.

This deliberately uses fresh render.sequence jobs, never a GUI's hidden history.
"""
import argparse
import copy
import hashlib
import json
import math
import os
from pathlib import Path
import re
import struct
import subprocess
import time
from uuid import uuid4

from .client import Client
from .models import guard


def canonical(value):
    return json.dumps(value, sort_keys=True, separators=(",", ":"), allow_nan=False).encode()


def digest(data):
    return hashlib.sha256(data).hexdigest()


def finite(value):
    return type(value) in (int, float) and math.isfinite(value)


def camera(value):
    if not isinstance(value, dict) or set(value) - {"eye", "target", "up", "fov_y", "near", "far", "projection", "ortho_height"}:
        raise ValueError("Invalid camera fields")
    result = dict(up=[0, 1, 0], near=.05, far=100, projection="perspective") | value
    if result["projection"] == "orthographic":
        if "fov_y" in value or not finite(value.get("ortho_height")) or value["ortho_height"] <= 0:
            raise ValueError("Orthographic requires positive ortho_height and forbids fov_y")
        result["primary_ray_origin"] = "near_plane"
    elif result["projection"] == "perspective":
        if "ortho_height" in value:
            raise ValueError("Perspective forbids ortho_height")
        result.setdefault("fov_y", 65)
        if not finite(result["fov_y"]) or not 1 < result["fov_y"] < 179:
            raise ValueError("Perspective requires 1 < fov_y < 179")
    else:
        raise ValueError("Invalid camera projection")
    for key in ("eye", "target", "up"):
        if key not in result or not isinstance(result[key], list) or len(result[key]) != 3 or not all(map(finite, result[key])):
            raise ValueError("Camera vectors require three finite numbers")
    if not all(finite(result[k]) for k in ("near", "far")) or not 0 < result["near"] < result["far"]:
        raise ValueError("Camera requires finite 0 < near < far")
    return result


def validate_manifest(manifest):
    if not isinstance(manifest, dict) or set(manifest) != {"schema_version", "project", "cases", "comparisons"} or manifest["schema_version"] != 1:
        raise ValueError("Expected render-case schema_version 1 with project, cases and comparisons")
    if not isinstance(manifest["project"], str) or not manifest["project"]:
        raise ValueError("Missing project manifest")
    cases = manifest["cases"]
    if not isinstance(cases, list) or not 1 <= len(cases) <= 32:
        raise ValueError("Expected 1..32 cases")
    seen = set()
    for case in cases:
        fields = {"id", "width", "height", "exposure", "render_options", "warmup_frames", "samples"}
        if not isinstance(case, dict) or set(case) not in (fields | {"preset"}, fields | {"environment"}):
            raise ValueError("Each case requires explicit rendering fields and exactly one preset/environment")
        if ("preset" in case and case["preset"] not in ("clear", "sparse", "overcast", "sunset")) or ("environment" in case and not isinstance(case["environment"], dict)):
            raise ValueError("Expected a known preset or complete environment object")
        if not isinstance(case["id"], str) or not re.fullmatch(r"[a-z][a-z0-9_-]{0,47}", case["id"]) or case["id"] in seen:
            raise ValueError("Case id must be unique and path-safe")
        seen.add(case["id"])
        if any(type(case[k]) is not int or not 1 <= case[k] <= 2048 for k in ("width", "height")):
            raise ValueError("Invalid case dimensions")
        if not finite(case["exposure"]) or not 0 <= case["exposure"] <= 1e6:
            raise ValueError("Invalid exposure")
        options = case["render_options"]
        if not isinstance(options, dict) or set(options) != {"mode", "cloud_shadows", "shadow_center_m", "shadow_extent_m", "shadow_resolution"}:
            raise ValueError("Explicit mode and complete shadow region required")
        if options["mode"] not in ("reference", "temporal") or type(options["cloud_shadows"]) is not bool:
            raise ValueError("Invalid render mode/shadow switch")
        center = options["shadow_center_m"]
        if not isinstance(center, list) or len(center) != 3 or not all(finite(v) and abs(v) <= 1e9 for v in center):
            raise ValueError("Invalid shadow center")
        resolution = options["shadow_resolution"]
        if type(resolution) is not int or not 16 <= resolution <= 512 or resolution & (resolution - 1):
            raise ValueError("Shadow resolution must be a bounded power of two")
        if not finite(options["shadow_extent_m"]) or not 16 <= options["shadow_extent_m"] <= 65536:
            raise ValueError("Invalid shadow extent")
        if type(case["warmup_frames"]) is not int or not 0 <= case["warmup_frames"] <= 64 or (options["mode"] == "reference" and case["warmup_frames"]):
            raise ValueError("Warmup requires temporal mode, at most 64 frames")
        samples = case["samples"]
        pixels = case["width"] * case["height"]
        if not isinstance(samples, list) or not 1 <= len(samples) <= 128 or pixels > 65536 or pixels * len(samples) > 262144:
            raise ValueError("Sequence sample/readback budget exceeded")
        for sample in samples:
            if not isinstance(sample, dict) or set(sample) != {"camera", "time_seconds", "camera_cut"}:
                raise ValueError("Samples require camera, time_seconds and camera_cut")
            camera(sample["camera"])
            if not finite(sample["time_seconds"]) or abs(sample["time_seconds"]) > 1e9 or type(sample["camera_cut"]) is not bool:
                raise ValueError("Invalid sample time/cut")
    if not isinstance(manifest["comparisons"], list) or len(manifest["comparisons"]) > 64:
        raise ValueError("Invalid comparison list")
    for pair in manifest["comparisons"]:
        if not isinstance(pair, dict) or set(pair) != {"reference", "candidate"} or any(pair[k] not in seen for k in pair) or pair["reference"] == pair["candidate"]:
            raise ValueError("Comparison requires two different known case ids")
    canonical(manifest)
    return manifest


def verify_submission(ticket, params, state, environment):
    for key in ("document_id", "scene_id", "revision"):
        if ticket.get(key) != state.get(key):
            raise ValueError("Submission scene identity mismatch: " + key)
    for key in ("output", "width", "height"):
        if ticket.get(key) != params[key]:
            raise ValueError("Submission request mismatch: " + key)
    exposure = struct.unpack("<f", struct.pack("<f", params["exposure"]))[0]
    expected = dict(environment=environment, exposure=exposure, render_options=params["render_options"],
                    warmup_frames=params["warmup_frames"], sample_count=len(params["samples"]),
                    camera=camera(params["samples"][0]["camera"]), time_seconds=params["samples"][0]["time_seconds"], profile="unlit_preview")
    if any(ticket.get("provenance", {}).get(k) != v for k, v in expected.items()):
        raise ValueError("Submission frozen inputs differ from requested case")


def verify_sequence(report, ticket, result, samples):
    fields = ("document_id", "scene_id", "revision", "frame", "width", "height")
    if result.get("kind") != "render_sequence" or report.get("schema_version") != 1 or report.get("kind") != "decker.render_sequence" or report.get("status") != "succeeded":
        raise ValueError("Invalid sequence result/report")
    for key in (*fields, "output"):
        if result.get(key) != ticket.get(key):
            raise ValueError("Sequence result differs from submission: " + key)
    for key in fields:
        if report.get(key) != ticket.get(key):
            raise ValueError("Sequence artifact identity mismatch: " + key)
    provenance = copy.deepcopy(ticket["provenance"])
    provenance["shader_fingerprint"] = result["provenance"].get("shader_fingerprint")
    if not isinstance(provenance["shader_fingerprint"], str) or not re.fullmatch(r"fnv1a64:[0-9a-f]{16}", provenance["shader_fingerprint"]):
        raise ValueError("Missing shader source fingerprint")
    if result["provenance"] != provenance or report.get("provenance") != provenance:
        raise ValueError("Frozen provenance mismatch")
    if not isinstance(report.get("device"), dict) or any(not isinstance(report["device"].get(k), str) or not report["device"][k] for k in ("name", "driver")):
        raise ValueError("Missing device/driver identity")
    if not finite(report.get("wall_ms")) or report["wall_ms"] < 0 or report.get("gpu_ms") is not None:
        raise ValueError("Invalid sequence timing source")
    frames = report.get("frames")
    pixels = ticket["width"] * ticket["height"]
    if not isinstance(frames, list) or len(frames) != len(samples):
        raise ValueError("Missing sequence frames")
    for i, (frame, sample) in enumerate(zip(frames, samples)):
        expected = dict(camera=camera(sample["camera"]), time_seconds=sample["time_seconds"], camera_cut=sample.get("camera_cut", False))
        if frame.get("index") != i or any(frame.get(k) != v for k, v in expected.items()):
            raise ValueError("Frame camera/time/cut mismatch")
        history = frame.get("history", {})
        if type(history.get("available")) is not bool or history.get("reset") not in ("none", "new_view", "disabled", "resize", "projection", "camera", "scene", "content", "quality", "time", "failure"):
            raise ValueError("Missing history state")
        if any(type(history.get(k)) is not int or history[k] < 0 for k in ("source_sequence", "candidate_sequence")) or type(frame.get("shadow_generation")) is not int or frame["shadow_generation"] < 0:
            raise ValueError("Invalid history/shadow identity")
        if provenance["render_options"]["mode"] == "reference" and (history["available"] or history["source_sequence"] or history["candidate_sequence"]):
            raise ValueError("Reference frame must not use temporal history")
        rgba, linear = frame.get("rgba8"), frame.get("linear_rgba")
        if not isinstance(rgba, list) or len(rgba) != pixels * 4 or any(type(b) is not int or not 0 <= b <= 255 for b in rgba):
            raise ValueError("Missing or invalid RGBA8 image")
        if not isinstance(linear, list) or len(linear) != pixels or any(not isinstance(p, list) or len(p) != 4 or not all(map(finite, p)) for p in linear):
            raise ValueError("Missing or nonfinite linear image")
        if not finite(frame.get("wall_ms")) or frame["wall_ms"] < 0 or frame.get("gpu_ms") is not None:
            raise ValueError("Invalid timing source; GPU time is not measured")
        summary = frame.get("linear_summary", {})
        expected_summary = {"min": [min(p[c] for p in linear) for c in range(3)],
                            "max": [max(p[c] for p in linear) for c in range(3)],
                            "mean": [sum(p[c] for p in linear) / pixels for c in range(3)]}
        for key, values in expected_summary.items():
            if key not in summary or len(summary[key]) != 3 or any(not finite(a) or abs(a-b) > 1e-6 * max(1, abs(b)) for a, b in zip(summary[key], values)):
                raise ValueError("Linear summary mismatch")
    return report


def compare(reference, candidate):
    for key in ("scene_id", "revision", "width", "height"):
        if reference[key] != candidate[key]:
            raise ValueError("Comparison scene/dimensions mismatch")
    a, b = reference["provenance"], candidate["provenance"]
    for key in ("environment", "exposure", "profile", "shader_fingerprint"):
        if a[key] != b[key]:
            raise ValueError("Comparison physical/source input mismatch: " + key)
    for key in ("cloud_shadows", "shadow_center_m", "shadow_extent_m", "shadow_resolution"):
        if a["render_options"][key] != b["render_options"][key]:
            raise ValueError("Comparison shadow input mismatch")
    if len(reference["frames"]) != len(candidate["frames"]):
        raise ValueError("Comparison frame count mismatch")
    metrics = []
    for x, y in zip(reference["frames"], candidate["frames"]):
        if any(x[k] != y[k] for k in ("camera", "time_seconds", "camera_cut")):
            raise ValueError("Comparison sample input mismatch")
        luminance = sorted(.2126*p[0]+.7152*p[1]+.0722*p[2] for p in x["linear_rgba"])
        scale = max(luminance[min(len(luminance)-1, int(.95*len(luminance)))], .001)
        errors = [abs(v[c]-u[c])/scale for u, v in zip(x["linear_rgba"], y["linear_rgba"]) for c in range(3)]
        ordered = sorted(errors)
        rms = math.sqrt(sum(e*e for e in errors)/len(errors))
        p99 = ordered[min(len(ordered)-1, int(.99*len(ordered)))]
        metrics.append(dict(index=x["index"], scale=scale, nrmse=rms, p99=p99, maximum=max(errors), passed=rms <= .05 and p99 <= .12))
    return dict(status="passed" if all(m["passed"] for m in metrics) else "failed", thresholds=dict(nrmse=.05, p99=.12), frames=metrics)


def static_variation(sequence):
    changes = []
    for a, b in zip(sequence["frames"], sequence["frames"][1:]):
        if a["camera"] != b["camera"] or a["time_seconds"] != b["time_seconds"] or b["camera_cut"]:
            continue
        luminance = sorted(.2126*p[0]+.7152*p[1]+.0722*p[2] for p in a["linear_rgba"])
        scale = max(luminance[min(len(luminance)-1, int(.95*len(luminance)))], .001)
        differences = [(v[c]-u[c])/scale for u, v in zip(a["linear_rgba"], b["linear_rgba"]) for c in range(3)]
        changes.append(math.sqrt(sum(d*d for d in differences)/len(differences)))
    return dict(static_pair_count=len(changes), maximum_nrms=max(changes) if changes else None, samples=changes)


def confined(root, relative):
    if not isinstance(relative, str) or Path(relative).is_absolute():
        raise ValueError("Expected a project-relative path")
    target = (root / relative).resolve()
    if not target.is_relative_to(root.resolve()):
        raise ValueError("Artifact escapes project root")
    return target


def run_cases(client, root, manifest, output, runner, timeout=240):
    """Run against an already loaded project. Scene edits are explicit and retained."""
    validate_manifest(manifest)
    root, runner = Path(root).resolve(), Path(runner).resolve()
    directory = confined(root, output)
    directory.mkdir(parents=True, exist_ok=False)
    binary = digest(runner.read_bytes())
    report = dict(schema_version=1, kind="decker.render_cases", manifest_sha256=digest(canonical(manifest)),
                  manifest=manifest, runner_sha256=binary, status="failed", cases=[], comparisons=[])
    successful = {}
    for case in manifest["cases"]:
        item = dict(id=case["id"], status="failed", error=None, input_sha256=digest(canonical(case)), artifacts=[], wall_ms=None)
        report["cases"].append(item)
        start = time.monotonic()
        try:
            query = client.call("environment.query").value
            environment = case.get("environment")
            if environment is None:
                preset = client.call("environment.preset", {"name": case["preset"]}).value
                if preset["version"] != 1:
                    raise ValueError("Unexpected preset version")
                environment = preset["environment"]
            state = client.call("environment.set", {"guard": guard(query["state"]), "environment": environment}).value["state"]
            normalized = client.call("environment.query").value
            if any(normalized["state"][k] != state[k] for k in ("document_id", "scene_id", "revision")):
                raise ValueError("Scene changed before case submission")
            params = {key: copy.deepcopy(case[key]) for key in ("width", "height", "exposure", "render_options", "warmup_frames", "samples")}
            params.update(guard=guard(state), output=f"{output}/{case['id']}.json", validation="required")
            ticket = client.call("render.sequence", params).value
            item["submission"] = ticket
            verify_submission(ticket, params, state, normalized["environment"])
            job = client.wait_job(ticket["job_id"], timeout=timeout)
            result = job["result"]
            artifact = confined(root, result["output"])
            raw = artifact.read_bytes()
            sequence = verify_sequence(json.loads(raw), ticket, result, params["samples"])
            if digest(runner.read_bytes()) != binary:
                raise ValueError("Runner binary changed during case execution")
            item["result"] = result
            item["report_sha256"] = digest(raw)
            item["static_variation"] = static_variation(sequence)
            for frame in sequence["frames"]:
                stem = f"{case['id']}-{frame['index']:03}"
                rgba = frame["rgba8"]
                rgb = bytes(v for i, v in enumerate(rgba) if i % 4 != 3)
                ppm = f"P6\n{case['width']} {case['height']}\n255\n".encode() + rgb
                linear = struct.pack("<" + "f" * (case["width"]*case["height"]*4), *(c for p in frame["linear_rgba"] for c in p))
                (directory / (stem + ".ppm")).write_bytes(ppm)
                (directory / (stem + ".rgba32f")).write_bytes(linear)
                item["artifacts"].append(dict(index=frame["index"], image=stem+".ppm", image_sha256=digest(ppm),
                                               linear=stem+".rgba32f", linear_sha256=digest(linear), summary=frame["linear_summary"]))
            successful[case["id"]] = sequence
            item["status"] = "succeeded"
            item["wall_ms"] = (time.monotonic()-start)*1000
        except Exception as error:
            item["error"] = dict(type=type(error).__name__, message=str(error))
            if hasattr(error, "job"):
                item["error"]["job"] = error.job
    for pair in manifest["comparisons"]:
        item = dict(pair, status="failed", error=None, metrics=None)
        try:
            if any(pair[key] not in successful for key in pair):
                raise ValueError("Missing successful comparison input")
            item["metrics"] = compare(successful[pair["reference"]], successful[pair["candidate"]])
            item["status"] = item["metrics"]["status"]
        except Exception as error:
            item["error"] = str(error)
        report["comparisons"].append(item)
    if all(c["status"] == "succeeded" for c in report["cases"]) and all(c["status"] == "passed" for c in report["comparisons"]):
        report["status"] = "passed"
    temporary = directory / "index.json.tmp"
    temporary.write_bytes(json.dumps(report, indent=2, allow_nan=False).encode())
    temporary.replace(directory / "index.json")
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for key in ("manifest", "project-root", "runner", "ctl"):
        parser.add_argument("--"+key, type=Path, required=True)
    parser.add_argument("--output", default="render-cases-"+uuid4().hex)
    args = parser.parse_args()
    manifest = validate_manifest(json.loads(args.manifest.read_text(encoding="utf-8")))
    root = args.project_root.resolve()
    confined(root, manifest["project"])
    endpoint = "dk-render-cases-"+uuid4().hex
    flags = subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0
    process = subprocess.Popen([str(args.runner.resolve()), "--project-root", str(root), "--pipe", endpoint],
                               stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, creationflags=flags)
    client = Client(endpoint, args.ctl.resolve(), timeout=30)
    try:
        deadline = time.monotonic()+30
        while True:
            try:
                client.hello(timeout=1)
                break
            except Exception:
                if process.poll() is not None or time.monotonic() > deadline:
                    raise RuntimeError("Runner failed to start")
                time.sleep(.05)
        client.call("scene.load", {"manifest": manifest["project"]})
        report = run_cases(client, root, manifest, args.output, args.runner)
        client.call("runtime.shutdown")
        _, stderr = process.communicate(timeout=30)
        if process.returncode or stderr:
            raise RuntimeError("Runner diagnostics: "+stderr.decode(errors="replace"))
        print(json.dumps(dict(status=report["status"], report=str(root/args.output/"index.json"))))
        return 0 if report["status"] == "passed" else 1
    finally:
        if process.poll() is None:
            process.kill()
            process.wait(timeout=10)
        process.stderr.close()


if __name__ == "__main__":
    raise SystemExit(main())
