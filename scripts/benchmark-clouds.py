"""M15.7 frozen cloud quality/performance evidence; missing observations are errors."""
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import statistics
import subprocess
import time
import uuid

ROOT = Path(__file__).resolve().parents[1]
PRESETS = ("clear", "sparse", "overcast", "sunset")
MODES = tuple(a+"-"+b for a in ("combined", "air", "unlit") for b in ("static", "motion"))
CPU_KEYS = ("extract_ms", "prepare_ms", "record_submit_ms", "wait_ms", "readback_ms", "end_to_end_ms")


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def distribution(values):
    if not values or any(not isinstance(x, (int, float)) or not math.isfinite(x) for x in values):
        raise ValueError("missing or nonfinite samples")
    ordered = sorted(values)
    return {"n": len(values), "p50": ordered[math.ceil(.5 * len(values)) - 1],
            "p95": ordered[math.ceil(.95 * len(values)) - 1], "min": ordered[0], "max": ordered[-1]}


def gpu(profile):
    if not isinstance(profile, dict) or profile["period_ns"] <= 0 or profile["valid_bits"] <= 0 or not profile["zones"]:
        raise ValueError("missing GPU observation")
    values = [profile["submission_ms"]] + [z[k] for z in profile["zones"] for k in ("gpu_ms", "cpu_record_ms")]
    if any(not math.isfinite(x) or x < 0 for x in values):
        raise ValueError("invalid GPU observation")
    return profile["submission_ms"]


def total_gpu(row):
    # Only nonoverlapping submission intervals; zones are attribution, never additions.
    profiles = [row["gpu"], *row["lut_builds"].values()]
    if row["shadow_updated"]:
        profiles.append(row["shadow_gpu"])
    values = [gpu(p) for p in profiles]
    ids = [p["submission_id"] for p in profiles]
    if len(set(ids)) != len(ids):
        raise ValueError("GPU submission counted twice")
    return sum(values)


def summarize(data):
    if data["schema"] != 1 or data["status"] != "passed" or data["kind"] != "performance" or data["validation"] or data["smoke"]:
        raise ValueError("failed, quality, smoke or validation-enabled performance run")
    if data["configuration"] != "RelWithDebInfo" or data["steady_samples"] != 20 or data["tracy_compiled"] or data["tracy_connected"]:
        raise ValueError("incompatible build or sample count")
    preset = data["preset"]
    if (preset["width"], preset["height"]) != (1920, 1080) or preset["name"] not in PRESETS or preset["quality"] != "cloud_temporal_v1":
        raise ValueError("incompatible preset or rendering quality")
    if preset["warmup"] != 32 or preset["shadow"] != dict(center_m=[0, 0, 0], extent_m=8192, resolution=256, steps=256):
        raise ValueError("quality inputs changed")
    clouds = preset["environment"]["clouds"]
    if (clouds["seed"], clouds["generator_version"], clouds["weather_resolution"], clouds["base_resolution"], clouds["detail_resolution"]) != (1729, 1, 128, 128, 32):
        raise ValueError("preset field inputs changed")
    if data["diagnostics"] != dict(errors=0, warnings=0) or data["cpu_heap_live_allocations"]:
        raise ValueError("diagnostic or CPU lifetime failure")
    if data["memory_device_only"]["all_allocations"] != data["memory_after_release"]["all_allocations"]:
        raise ValueError("GPU lifetime failure")
    frames = data["frames"]
    if len(frames) != 2*53+4*26:
        raise ValueError("incomplete matrix")
    groups = {}
    for mode in MODES:
        combined, air, motion = mode.startswith("combined"), not mode.startswith("unlit"), mode.endswith("motion")
        cloudy = combined and clouds["enabled"]
        warmup = 32 if combined else 5
        rows = [f for f in frames if f["mode"] == mode]
        if [f["index"] for f in rows] != list(range(1+warmup+20)):
            raise ValueError("missing or duplicate frame")
        for row in rows:
            i = row["index"]
            phase = "cold" if i == 0 else "warmup" if i <= warmup else "steady"
            if row["phase"] != phase or row["logical_index"] != i-warmup-1:
                raise ValueError("incorrect frame phase")
            step = max(0, i-warmup) if motion else 0
            if row["eye_m"] != [step*.25, 100, 0] or row["yaw_rad"] != step*.0005 or abs(row["time_seconds"]-step/30) > 1e-12:
                raise ValueError("camera/time input mismatch")
            if any(not math.isfinite(row[k]) or row[k] < 0 for k in CPU_KEYS) or abs(sum(row[k] for k in CPU_KEYS[:-1])-row["end_to_end_ms"]) > .001:
                raise ValueError("invalid CPU interval")
            expected = ({"transmittance", "multiple_scattering", "sky_view"} if mode == "combined-static" and i == 0 else
                        {"sky_view"} if air and (motion and step > 0 or mode == "air-static" and i == 0) else set())
            if set(row["lut_builds"]) != expected:
                raise ValueError("incorrect LUT invalidation")
            shadow_update = cloudy and (mode == "combined-static" and i == 0 or motion and step > 0)
            if row["shadow_updated"] != shadow_update or bool(row["shadow_generation"]) != bool(cloudy):
                raise ValueError("missing or unexpected shadow update")
            if bool(row["history"]["available"]) != bool(cloudy and i > 0):
                raise ValueError("incorrect history continuity")
            if data["timestamps"]:
                total_gpu(row)
                names = {z["name"] for z in row["gpu"]["zones"]}
                needed = ({"half resolution cloud transport", "depth preserving cloud reconstruction"} if cloudy else
                          {"aerial perspective volume", "sky and geometry composition"} if air else {"opaque"})
                if not needed <= names:
                    raise ValueError("missing render passes")
            elif row["gpu"] is not None or row["shadow_gpu"] is not None or any(p is not None for p in row["lut_builds"].values()):
                raise ValueError("unexpected OFF timestamp")
            if not shadow_update and row["shadow_gpu"] is not None:
                raise ValueError("cached shadow counted as new work")
        groups[mode] = [r for r in rows if r["phase"] == "steady"]
    result = dict(initialization_ms={k: data[k] for k in ("device_queue_ms", "program_geometry_create_ms", "cpu_field_ms", "texture_upload_wall_ms")},
                  cold_frames=[r for r in frames if r["phase"] == "cold"], modes={}, increments={},
                  cpu_heap_peak_bytes=data["cpu_heap_peak_bytes"], memory_geometry_only=data["memory_geometry_only"], memory_samples=data["memory_samples"])
    for mode, rows in groups.items():
        stats = {key: distribution([r[key] for r in rows]) for key in CPU_KEYS}
        if data["timestamps"]:
            stats["total_gpu_ms"] = distribution([total_gpu(r) for r in rows])
            stats["frame_gpu_ms"] = distribution([gpu(r["gpu"]) for r in rows])
            stats["shadow_gpu_ms"] = distribution([gpu(r["shadow_gpu"]) if r["shadow_updated"] else 0 for r in rows])
            stats["lut_gpu_ms"] = distribution([sum(gpu(p) for p in r["lut_builds"].values()) for r in rows])
            names = sorted({z["name"] for r in rows for z in r["gpu"]["zones"]})
            stats["zones"] = {name: distribution([sum(z["gpu_ms"] for z in r["gpu"]["zones"] if z["name"] == name) for r in rows]) for name in names}
        result["modes"][mode] = stats
    if data["timestamps"]:
        for motion in ("static", "motion"):
            c, a, u = (groups[k+"-"+motion] for k in ("combined", "air", "unlit"))
            for rows in zip(c, a, u, strict=True):
                if any(rows[0][k] != row[k] for row in rows[1:] for k in ("eye_m", "yaw_rad", "time_seconds", "logical_index")):
                    raise ValueError("baseline input mismatch")
            result["increments"][motion] = dict(
                cloud_gpu_ms=distribution([total_gpu(x)-total_gpu(y) for x, y in zip(c, a, strict=True)]),
                combined_gpu_ms=distribution([total_gpu(x)-total_gpu(y) for x, y in zip(c, u, strict=True)]))
    memories = data["memory_samples"]
    if [m["mode"] for m in memories] != list(MODES):
        raise ValueError("missing memory diagnostic")
    for m in memories:
        for key in ("inflight_with_old_frame", "steady_cache_history"):
            if not isinstance(m.get(key), dict) or not m[key] or any(type(v) is not int or v < 0 for v in m[key].values()):
                raise ValueError("invalid memory observation")
    base = data["memory_geometry_only"]["device_local_allocation_bytes"]
    result["environment_steady_bytes"] = max(m["steady_cache_history"]["device_local_allocation_bytes"]-base for m in memories if m["mode"].startswith("combined"))
    result["environment_sampled_peak_bytes"] = max(m["inflight_with_old_frame"]["device_local_allocation_bytes"]-base for m in memories if m["mode"].startswith("combined"))
    return result


def matrix_candidates(runs):
    result = {}
    for preset in PRESETS:
        chosen = [r for r in runs if r["preset"] == preset and r["timestamps"]]
        if sorted(r["round"] for r in chosen) != [1, 2, 3]:
            raise ValueError("incomplete three-round candidate set")
        result[preset] = {}
        for key, limit in (("cloud_gpu_ms", 4), ("combined_gpu_ms", 5.5)):
            values = [r["increments"]["motion"][key]["p95"] for r in chosen]
            result[preset][key] = dict(candidate_p95_ms=limit, round_p95_ms=values, median_p95_ms=statistics.median(values), met_all_rounds=all(v <= limit for v in values))
        for key, limit in (("environment_steady_bytes", 128*1024**2), ("environment_sampled_peak_bytes", 192*1024**2)):
            values = [r[key] for r in chosen]
            result[preset][key] = dict(candidate_bytes=limit, round_bytes=values, met_all_rounds=all(v <= limit for v in values))
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    binary = (args.build_dir/"bin/RelWithDebInfo/dk-cloud-benchmark.exe").resolve()
    if not binary.is_file():
        parser.error("build dk_cloud_benchmark RelWithDebInfo first")
    directory = args.output or ROOT/"out/benchmarks"/(time.strftime("clouds-%Y%m%d-%H%M%S-")+uuid.uuid4().hex[:8])
    directory.mkdir(parents=True, exist_ok=False)
    sources = [p for folder in ("engine", "shaders", "cmake") for p in (ROOT/folder).rglob("*") if p.is_file()]
    sources += [ROOT/p for p in ("CMakeLists.txt", "CMakePresets.json", "vcpkg.json", "vcpkg-configuration.json", "tests/integration/CMakeLists.txt",
                                "tests/integration/CloudBenchmark.cpp", "tests/integration/CloudBenchmarkSupport.hpp", "tests/integration/CloudQualityCases.hpp", "tests/RenderAssetSupport.hpp")]
    sources.append(Path(__file__))
    hashes = {p.relative_to(ROOT).as_posix(): digest(p) for p in sources}
    binaries = {p.name: digest(p) for p in binary.parent.glob("*.dll")}; binaries[binary.name] = digest(binary)
    manifest = dict(schema=1, status="running", started_at=time.strftime("%Y-%m-%dT%H:%M:%S%z"),
                    commit=subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip(),
                    working_tree=subprocess.check_output(["git", "status", "--short"], cwd=ROOT, text=True),
                    binary=str(binary), binaries_sha256=binaries, sources_sha256=hashes,
                    measurement_boundary="offscreen synchronous API including readback, excludes GUI/IPC/files; nonoverlapping GPU submissions", runs=[])
    env = os.environ.copy(); env["DISABLE_LAYER_AMD_SWITCHABLE_GRAPHICS_1"] = "1"
    for key in ("VK_LAYER_VALIDATE_SYNC", "VK_VALIDATION_VALIDATE_SYNC"):
        env.pop(key, None)
    summaries, inputs = [], {}
    def save_manifest():
        (directory/"manifest.json").write_text(json.dumps(manifest, indent=2), encoding="utf-8")
    save_manifest()
    try:
        for round_index in range(3):
            for preset in PRESETS[round_index:]+PRESETS[:round_index]:
                for timestamps in ((False, True) if round_index % 2 == 0 else (True, False)):
                    name = f"r{round_index+1}-{preset}-{'timestamps' if timestamps else 'off'}"
                    path = directory/(name+".json")
                    entry = dict(name=name, round=round_index+1, preset=preset, timestamps=timestamps, status="running")
                    manifest["runs"].append(entry); save_manifest(); print(name, flush=True)
                    command = [str(binary), "--preset", preset, "--output", str(path)]+(["--timestamps"] if timestamps else [])
                    with (directory/(name+".log")).open("w", encoding="utf-8") as log:
                        completed = subprocess.run(command, cwd=ROOT, env=env, stdout=log, stderr=subprocess.STDOUT, timeout=1200)
                    if completed.returncode:
                        raise RuntimeError(f"{name} failed: {completed.returncode}")
                    data = json.loads(path.read_text(encoding="utf-8")); summary = summarize(data)
                    if preset in inputs and inputs[preset] != data["preset"]:
                        raise ValueError("preset physical inputs changed between runs")
                    inputs[preset] = data["preset"]
                    entry.update(status="passed", raw_sha256=digest(path), image_sha256=digest(path.with_suffix(".ppm")))
                    summaries.append(dict(entry, **summary, device=data["device"], compiler=data["compiler"], shader_fingerprint=data["shader_fingerprint"]))
                    save_manifest()
        if any(digest(ROOT/p) != value for p, value in hashes.items()) or any(digest(binary.parent/p) != value for p, value in binaries.items()):
            raise ValueError("source or binary changed during matrix")
        if len({json.dumps(s["device"], sort_keys=True) for s in summaries}) != 1 or len({s["shader_fingerprint"] for s in summaries}) != 1:
            raise ValueError("device or shader changed during matrix")
        for preset in PRESETS:
            if len({r["image_sha256"] for r in manifest["runs"] if r["preset"] == preset}) != 1:
                raise ValueError("timestamp toggle or repeat changed the fixed-input image")
        overhead = {preset: [dict(round=round_index, end_to_end_p95_ratio=(
            next(r for r in summaries if r["preset"] == preset and r["round"] == round_index and r["timestamps"])["modes"]["combined-motion"]["end_to_end_ms"]["p95"] /
            next(r for r in summaries if r["preset"] == preset and r["round"] == round_index and not r["timestamps"])["modes"]["combined-motion"]["end_to_end_ms"]["p95"]))
            for round_index in (1, 2, 3)] for preset in PRESETS}
        summary = dict(schema=1, status="passed", units="ms or bytes", presets=inputs, timestamp_overhead=overhead,
                       performance_candidates=matrix_candidates(summaries), runs=summaries)
        (directory/"summary.json").write_text(json.dumps(summary, indent=2), encoding="utf-8")
        manifest["status"] = "passed"
    except Exception as error:
        manifest.update(status="failed", error=str(error))
        if manifest["runs"] and manifest["runs"][-1]["status"] == "running":
            manifest["runs"][-1].update(status="failed", error=str(error))
        raise
    finally:
        manifest["finished_at"] = time.strftime("%Y-%m-%dT%H:%M:%S%z"); save_manifest(); print(directory, flush=True)


if __name__ == "__main__":
    main()
