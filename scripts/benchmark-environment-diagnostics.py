"""Three-round cloud attribution with fixed-input single-factor comparisons.

Runtime defaults are unchanged. This diagnostic changes sampling quality and
must not be treated as a quality acceptance or an optimization recommendation.
"""
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import subprocess
import time

ROOT = Path(__file__).resolve().parents[1]
CPU_KEYS = ("extract_ms", "prepare_ms", "record_submit_ms", "wait_ms", "readback_ms", "end_to_end_ms")
CASES = [(f"baseline-{preset}", preset, [], True) for preset in ("clear", "sparse", "overcast", "sunset")]
CASES += [
    ("720p", "overcast", ["--width", "1280", "--height", "720", "--mode", "combined-static"], True),
    ("540p", "overcast", ["--width", "960", "--height", "540", "--mode", "combined-static"], True),
    ("field-half", "overcast", ["--field-divisor", "2", "--mode", "combined-static"], True),
    ("primary-48", "overcast", ["--steps", "48", "--mode", "combined-static"], True),
    ("sun-4", "overcast", ["--sun-steps", "4", "--mode", "combined-static"], True),
    ("no-self-shadow", "overcast", ["--no-self-shadow", "--mode", "combined-static"], True),
    ("no-ground-shadow", "overcast", ["--no-shadow", "--mode", "combined-motion"], True),
    ("shadow-128", "overcast", ["--shadow-resolution", "128", "--mode", "combined-motion"], True),
    ("shadow-64-steps", "overcast", ["--shadow-steps", "64", "--mode", "combined-motion"], True),
    ("timestamps-off", "overcast", [], False),
]


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def distribution(values):
    if not values or any(not math.isfinite(v) or v < 0 for v in values):
        raise ValueError("Missing, negative or nonfinite measurement")
    values = sorted(values)
    return dict(n=len(values), p50=values[math.ceil(.5 * len(values)) - 1],
                p95=values[math.ceil(.95 * len(values)) - 1], min=values[0], max=values[-1])


def gpu(profile):
    if not profile or profile["period_ns"] <= 0 or profile["valid_bits"] <= 0:
        raise ValueError("GPU timestamp missing")
    return profile["submission_ms"]


def summarize(data):
    if data["status"] != "passed" or data["validation"] or data["tracy_compiled"] or data["smoke"]:
        raise ValueError("Not a valid independent performance run")
    if data["configuration"] not in ("RelWithDebInfo", "Release"):
        raise ValueError("Performance collection requires an optimized build")
    if data["diagnostics"] != dict(errors=0, warnings=0) or data["cpu_heap_live_allocations"]:
        raise ValueError("Diagnostics or CPU lifetime failure")
    if data["memory_after_release"]["all_allocations"] != data["memory_device_only"]["all_allocations"]:
        raise ValueError("GPU lifetime failure")
    result = {k: data[k] for k in ("device", "configuration", "compiler", "preset", "diagnostic_options", "timestamps",
        "device_queue_ms", "program_geometry_create_ms", "cpu_field_ms", "texture_upload_wall_ms", "field",
        "memory_device_only", "memory_geometry_only", "memory_field_textures", "cpu_heap_peak_bytes", "memory_samples")}
    result["work_diagnostics"] = data.get("work_diagnostics", {})
    result["modes"] = {}
    for mode in dict.fromkeys(row["mode"] for row in data["frames"]):
        frames = [r for r in data["frames"] if r["mode"] == mode]
        rows = [r for r in frames if r["phase"] == "steady"]
        if len(rows) != data["steady_samples"] or [r["index"] for r in frames] != list(range(len(frames))):
            raise ValueError("Incomplete or duplicate frames")
        for row in frames:
            if abs(sum(row[k] for k in CPU_KEYS[:-1]) - row["end_to_end_ms"]) > .001:
                raise ValueError("Inconsistent CPU measurement intervals")
        stats = {k: distribution([r[k] for r in rows]) for k in CPU_KEYS}
        if data["timestamps"]:
            stats["frame_gpu_ms"] = distribution([gpu(r["gpu"]) for r in rows])
            stats["lut_gpu_ms"] = distribution([sum(gpu(p) for p in r["lut_builds"].values()) for r in rows])
            stats["shadow_gpu_ms"] = distribution([gpu(r["shadow_gpu"]) if r["shadow_gpu"] else 0 for r in rows])
            stats["total_gpu_ms"] = distribution([gpu(r["gpu"]) + sum(gpu(p) for p in r["lut_builds"].values())
                + (gpu(r["shadow_gpu"]) if r["shadow_gpu"] else 0) for r in rows])
            names = sorted({z["name"] for r in rows for z in r["gpu"]["zones"]})
            stats["passes"] = {name: distribution([sum(z["gpu_ms"] for z in r["gpu"]["zones"] if z["name"] == name) for r in rows]) for name in names}
        result["modes"][mode] = dict(steady=stats, cold=frames[0])
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--configuration", choices=("RelWithDebInfo", "Release"), default="RelWithDebInfo")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    binary = (args.build_dir / "bin" / args.configuration / "dk-cloud-benchmark.exe").resolve()
    if not binary.is_file():
        parser.error("Build dk_cloud_benchmark first")
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    paths = [p for folder in ("engine", "shaders") for p in (ROOT / folder).rglob("*") if p.is_file()]
    paths += [Path(__file__), ROOT / "tests/integration/CloudBenchmark.cpp", ROOT / "tests/integration/CloudBenchmarkSupport.hpp"]
    hashes = {str(p.relative_to(ROOT)): digest(p) for p in paths}
    binaries = {p.name: digest(p) for p in binary.parent.glob("*.dll")}
    binaries[binary.name] = digest(binary)
    manifest = dict(status="running", started_at=time.strftime("%Y-%m-%dT%H:%M:%S%z"),
        commit=subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip(),
        working_tree=subprocess.check_output(["git", "status", "--short"], cwd=ROOT, text=True),
        sources_sha256=hashes, binaries_sha256=binaries, binary=str(binary), runs=[],
        boundary="Synchronous offscreen API, readback included; extra work counters excluded from timing/memory samples. No GUI or file publication.")
    (output / "manifest.json").write_text(json.dumps(manifest, indent=2), encoding="utf-8")
    env = os.environ.copy()
    env["DISABLE_LAYER_AMD_SWITCHABLE_GRAPHICS_1"] = "1"
    for key in ("VK_LAYER_VALIDATE_SYNC", "VK_VALIDATION_VALIDATE_SYNC"):
        env.pop(key, None)
    telemetry = None
    summaries = []
    with (output / "gpu-telemetry.csv").open("w") as telemetry_out, (output / "gpu-telemetry-error.log").open("w") as telemetry_err:
        try:
            try:
                telemetry = subprocess.Popen(["nvidia-smi", "--query-gpu=timestamp,name,driver_version,memory.used,power.draw,temperature.gpu,utilization.gpu,clocks.gr,clocks.mem",
                    "--format=csv", "-lms", "500"], stdout=telemetry_out, stderr=telemetry_err, creationflags=subprocess.CREATE_NO_WINDOW)
            except FileNotFoundError:
                manifest["telemetry"] = "nvidia-smi unavailable; no hardware utilization measurements"
            for round_index in range(3):
                cases = CASES[round_index:] + CASES[:round_index]
                for name, preset, extra, timestamps in cases:
                    run = f"r{round_index + 1}-{name}"
                    path = output / (run + ".json")
                    command = [str(binary), "--preset", preset, "--output", str(path), *extra]
                    if timestamps:
                        command += ["--timestamps"]
                    entry = dict(name=name, round=round_index + 1, command=command, started_at=time.time())
                    print(run, flush=True)
                    with (output / (run + ".log")).open("w", encoding="utf-8") as log:
                        subprocess.run(command, cwd=ROOT, env=env, stdout=log, stderr=subprocess.STDOUT, check=True, timeout=300,
                                       creationflags=subprocess.CREATE_NO_WINDOW)
                    entry["finished_at"] = time.time()
                    data = json.loads(path.read_text(encoding="utf-8"))
                    summary = summarize(data)
                    entry["raw_sha256"] = digest(path)
                    manifest["runs"].append(entry)
                    summaries.append(dict(name=name, round=round_index + 1, **summary))
                    (output / "summary.json").write_text(json.dumps(summaries, indent=2), encoding="utf-8")
                    (output / "manifest.json").write_text(json.dumps(manifest, indent=2), encoding="utf-8")
            if any(digest(ROOT / p) != value for p, value in hashes.items()) or any(digest(binary.parent / p) != value for p, value in binaries.items()):
                raise RuntimeError("Measured source or binaries changed during collection")
            manifest["status"] = "passed"
        except Exception as error:
            manifest["status"] = "failed"
            manifest["error"] = str(error)
            raise
        finally:
            if telemetry is not None:
                telemetry.terminate()
                telemetry.wait(timeout=15)
            manifest["finished_at"] = time.strftime("%Y-%m-%dT%H:%M:%S%z")
            (output / "manifest.json").write_text(json.dumps(manifest, indent=2), encoding="utf-8")
    print(f"Completed {len(summaries)} independent runs: {output}", flush=True)


if __name__ == "__main__":
    main()
