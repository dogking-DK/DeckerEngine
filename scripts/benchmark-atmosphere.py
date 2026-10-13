"""M14.7 fixed 1080p OFF/timestamp matrix. Build/quality verification is separate."""
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
PRESETS = ("noon", "sunset", "high-altitude")
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


def summarize(data):
    if data["schema"] != 1 or data["status"] != "passed" or data["validation"]:
        raise ValueError("failed or validation-enabled performance run")
    if data["configuration"] != "RelWithDebInfo" or data["steady_samples"] != 40 or data["tracy_compiled"] or data["tracy_connected"]:
        raise ValueError("incompatible build or sample count")
    if (data["preset"]["width"], data["preset"]["height"]) != (1920, 1080) or data["preset"]["name"] not in PRESETS:
        raise ValueError("incompatible preset")
    if data["diagnostics"] != {"errors": 0, "warnings": 0} or data["cpu_heap_live_allocations"]:
        raise ValueError("diagnostic or lifetime failure")
    if data["memory_device_only"]["all_allocations"] != data["memory_after_release"]["all_allocations"]:
        raise ValueError("GPU lifetime failure")
    frames = data["frames"]
    if len(frames) != 46 * 3 + 3:
        raise ValueError("incomplete matrix")
    groups = {}
    for mode in ("static", "sky-update", "physical-update", "unlit"):
        rows = [f for f in frames if f["mode"] == mode]
        if [f["index"] for f in rows] != list(range(3 if mode == "physical-update" else 46)):
            raise ValueError("missing or repeated frame")
        for row in rows:
            i = row["index"]
            if row["phase"] != ("rebuild" if mode == "physical-update" else "cold" if i == 0 else "warmup" if i < 6 else "steady"):
                raise ValueError("incorrect phase")
            if any(not math.isfinite(row[k]) or row[k] < 0 for k in CPU_KEYS):
                raise ValueError("invalid CPU observation")
            if abs(sum(row[k] for k in CPU_KEYS[:-1]) - row["end_to_end_ms"]) > .001:
                raise ValueError("inconsistent end-to-end interval")
            expected_luts = ({"transmittance", "multiple_scattering", "sky_view"} if mode == "physical-update" or mode == "static" and i == 0
                             else {"sky_view"} if mode == "sky-update" else set())
            if set(row["lut_builds"]) != expected_luts:
                raise ValueError("incorrect cache invalidation")
            if data["timestamps"]:
                gpu(row["gpu"])
                for observation in row["lut_builds"].values():
                    gpu(observation)
                names = {z["name"] for z in row["gpu"]["zones"]}
                if mode != "unlit" and not {"aerial perspective volume", "sky and geometry composition"} <= names:
                    raise ValueError("missing atmosphere passes")
            elif row["gpu"] is not None or any(p is not None for p in row["lut_builds"].values()):
                raise ValueError("unexpected timestamp in OFF run")
        groups[mode] = rows
    result = {"initialization_ms": {k: data[k] for k in ("device_queue_ms", "pipeline_create_ms", "atmosphere_create_ms")},
              "cold": groups["static"][0], "modes": {}, "memory_cache_only": data["memory_cache_only"],
              "cpu_heap_peak_bytes": data["cpu_heap_peak_bytes"]}
    for mode, rows in groups.items():
        selected = [r for r in rows if r["phase"] in ("steady", "rebuild")]
        stats = {k: distribution([r[k] for r in selected]) for k in CPU_KEYS}
        if data["timestamps"]:
            stats["frame_gpu_ms"] = distribution([gpu(r["gpu"]) for r in selected])
            if mode != "unlit":
                stats["lut_gpu_ms"] = distribution([sum(gpu(p) for p in r["lut_builds"].values()) for r in selected])
                for name in ("aerial perspective volume", "sky and geometry composition"):
                    stats[name] = distribution([sum(z["gpu_ms"] for z in r["gpu"]["zones"] if z["name"] == name) for r in selected])
            if mode in ("static", "sky-update"):
                baseline = [r for r in groups["unlit"] if r["phase"] == "steady"]
                stats["incremental_gpu_ms"] = distribution([gpu(r["gpu"]) + sum(gpu(p) for p in r["lut_builds"].values()) - gpu(b["gpu"])
                                                           for r, b in zip(selected, baseline, strict=True)])
        result["modes"][mode] = stats
    memories = [r["with_frame"] for r in data["memory_samples"]]
    if len(memories) != len(frames):
        raise ValueError("missing memory samples")
    memories.append(data["memory_inflight_frame"])
    result["memory_inflight_frame"] = data["memory_inflight_frame"]
    result["vma_sampled_peak"] = {key: max(row[key] for row in memories) for key in memories[0]}
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    binary = (args.build_dir / "bin/RelWithDebInfo/dk-atmosphere-benchmark.exe").resolve()
    if not binary.is_file():
        parser.error("build dk_atmosphere_benchmark RelWithDebInfo first")
    directory = args.output or ROOT / "out/benchmarks" / (time.strftime("atmosphere-%Y%m%d-%H%M%S-") + uuid.uuid4().hex[:8])
    directory.mkdir(parents=True, exist_ok=False)
    sources = [p for folder in ("engine", "shaders", "cmake") for p in (ROOT / folder).rglob("*") if p.is_file()]
    sources += [ROOT / "CMakeLists.txt", ROOT / "CMakePresets.json", ROOT / "vcpkg.json", ROOT / "vcpkg-configuration.json",
                ROOT / "tests/integration/AtmosphereBenchmark.cpp", ROOT / "tests/integration/CMakeLists.txt", Path(__file__)]
    source_hashes = {p.relative_to(ROOT).as_posix(): digest(p) for p in sources if p.is_file()}
    binaries = {p.name: digest(p) for p in binary.parent.glob("*.dll")}
    binaries[binary.name] = digest(binary)
    manifest = {"schema": 1, "status": "running", "started_at": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
                "commit": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip(),
                "working_tree": subprocess.check_output(["git", "status", "--short"], cwd=ROOT, text=True),
                "binary": str(binary), "binaries_sha256": binaries, "sources_sha256": source_hashes,
                "measurement_boundary": "synchronous offscreen API including readback; no GUI/IPC/file publication",
                "runs": []}
    (directory / "manifest.json").write_text(json.dumps(manifest, indent=2), encoding="utf-8")
    env = os.environ.copy()
    env["DISABLE_LAYER_AMD_SWITCHABLE_GRAPHICS_1"] = "1"
    for key in ("VK_LAYER_VALIDATE_SYNC", "VK_VALIDATION_VALIDATE_SYNC"):
        env.pop(key, None)
    summaries = []
    try:
        for round_index in range(3):
            for preset in PRESETS[round_index:] + PRESETS[:round_index]:
                for timestamps in ((False, True) if round_index % 2 == 0 else (True, False)):
                    name = f"r{round_index + 1}-{preset}-{'timestamps' if timestamps else 'off'}"
                    path = directory / (name + ".json")
                    command = [str(binary), "--preset", preset, "--output", str(path)] + (["--timestamps"] if timestamps else [])
                    print(name, flush=True)
                    with (directory / (name + ".log")).open("w", encoding="utf-8") as log:
                        completed = subprocess.run(command, cwd=ROOT, env=env, stdout=log, stderr=subprocess.STDOUT, timeout=300)
                    if completed.returncode:
                        raise RuntimeError(f"{name} failed: {completed.returncode}")
                    data = json.loads(path.read_text(encoding="utf-8"))
                    summary = summarize(data)
                    entry = {"name": name, "round": round_index + 1, "preset": preset, "timestamps": timestamps,
                             "raw_sha256": digest(path), "image_sha256": digest(path.with_suffix(".ppm"))}
                    manifest["runs"].append(entry)
                    summaries.append({**entry, **summary, "device": data["device"], "compiler": data["compiler"], "shader_fingerprint": data["shader_fingerprint"]})
        if any(digest(ROOT / p) != value for p, value in source_hashes.items()) or any(digest(binary.parent / p) != value for p, value in binaries.items()):
            raise ValueError("source or binary changed during matrix")
        if len({json.dumps(s["device"], sort_keys=True) for s in summaries}) != 1 or len({s["shader_fingerprint"] for s in summaries}) != 1:
            raise ValueError("device or shader changed during matrix")
        frozen = {}
        for preset in PRESETS:
            runs = [s for s in summaries if s["preset"] == preset and s["timestamps"]]
            p95s = [s["modes"]["sky-update"]["incremental_gpu_ms"]["p95"] for s in runs]
            frozen[preset] = {"candidate_gpu_p95_ms": 1.5, "observed_round_p95_ms": p95s,
                              "median_p95_ms": statistics.median(p95s), "candidate_met_all_rounds": all(x <= 1.5 for x in p95s)}
        (directory / "summary.json").write_text(json.dumps({"schema": 1, "status": "passed", "units": "ms or bytes",
                                                           "performance_candidate": frozen, "runs": summaries}, indent=2), encoding="utf-8")
        manifest["status"] = "passed"
    except Exception as error:
        manifest["status"] = "failed"
        manifest["error"] = str(error)
        raise
    finally:
        manifest["finished_at"] = time.strftime("%Y-%m-%dT%H:%M:%S%z")
        (directory / "manifest.json").write_text(json.dumps(manifest, indent=2), encoding="utf-8")
        print(directory, flush=True)


if __name__ == "__main__":
    main()
