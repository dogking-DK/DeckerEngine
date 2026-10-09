"""M11.2 fixed workload, rotated independent runs, real Tracy and Named Pipe evidence."""
import argparse
import csv
from datetime import datetime
import hashlib
import importlib.util
import io
import json
import math
import os
from pathlib import Path
import platform
import statistics
import subprocess
import time
from uuid import uuid4

ROOT = Path(__file__).resolve().parents[1]
MODES = ("off", "timestamps", "tracy")
SIZES = (8, 16, 32)
FLAGS = subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0


def save(path, value):
    path.write_text(json.dumps(value, indent=2, ensure_ascii=False, allow_nan=False) + "\n", encoding="utf-8")


def distribution(values):
    values = list(values)
    if not values or any(type(v) not in (int, float) or not math.isfinite(v) or v < 0 for v in values):
        raise ValueError("missing, negative or nonfinite observations")
    values.sort()
    return {"n": len(values), **{f"p{p}": values[max(0, math.ceil(len(values) * p / 100) - 1)] for p in (50, 95, 99)},
            "min": values[0], "max": values[-1]}


def run(command, directory, label, timeout=180, env=None):
    with (directory / (label + ".stdout.log")).open("wb") as out, (directory / (label + ".stderr.log")).open("wb") as err:
        process = subprocess.Popen([str(x) for x in command], cwd=ROOT, stdin=subprocess.DEVNULL,
                                   stdout=out, stderr=err, creationflags=FLAGS, env=env)
        try:
            code = process.wait(timeout=timeout)
            if code:
                raise RuntimeError(f"{label} exited {code}: {directory}")
        finally:
            if process.poll() is None:
                process.kill()
                process.wait(timeout=10)


def trace_run(command, tools, directory, env):
    with (directory / "probe.stdout.log").open("wb") as out, (directory / "probe.stderr.log").open("wb") as err:
        probe = subprocess.Popen([str(x) for x in command] + ["--capture"], cwd=ROOT, stdin=subprocess.DEVNULL,
                                 stdout=out, stderr=err, creationflags=FLAGS, env=env)
        try:
            run([tools / "tracy-capture.exe", "-a", "127.0.0.1", "-p", env["TRACY_PORT"],
                 "-o", directory / "capture.tracy"], directory, "capture", env=env)
            if probe.wait(timeout=30):
                raise RuntimeError("Tracy benchmark failed: " + str(directory))
        finally:
            if probe.poll() is None:
                probe.kill()
                probe.wait(timeout=10)
    for kind, flag in (("cpu", "-u"), ("gpu", "-g")):
        run([tools / "tracy-csvexport.exe", flag, directory / "capture.tracy"], directory, kind, env=env)
        (directory / (kind + ".stdout.log")).rename(directory / (kind + ".csv"))


def read_measurement(path):
    m = json.loads(path.read_text(encoding="utf-8"))
    if (m["schema"] != 1 or m["status"] != "passed" or m["steps"] != 300 or m["seed"] != 42
            or m["dt_ns"] != 10000000 or m["iterations"] != 12 or m["size"] not in SIZES
            or m["configuration"] != "RelWithDebInfo" or m["validation"]):
        raise ValueError("incompatible or failed measurement: " + str(path))
    expected = 0
    for b in m["batches"]:
        count = min(8, 300 - expected)
        phase = "cold" if expected == 0 else "tail" if count != 8 else "warmup" if expected < 32 else "steady"
        if b["first_step"] != expected or b["count"] != count or b["phase"] != phase:
            raise ValueError("noncontiguous steps or incorrect phase")
        distribution([b["wall_ns"]])
        if m["timestamps"]:
            if b["gpu"] is None or b["gpu"]["zones"] < 2:
                raise ValueError("missing GPU observations")
            distribution([b["gpu"][key] for key in ("submission_ns", "compute_ns", "draw_ns", "transfer_ns")])
        elif b["gpu"] is not None:
            raise ValueError("unexpected GPU observations")
        expected += count
    if expected != 300 or len(m["particles"]) != m["size"] ** 2:
        raise ValueError("wrong steps or particles")
    if any(not math.isfinite(x) for p in m["particles"] for x in p):
        raise ValueError("nonfinite particle")
    return m


def cpu_phases(directory):
    rows = list(csv.DictReader((directory / "cpu.csv").open(encoding="utf-8-sig", newline="")))
    gpu = list(csv.DictReader((directory / "gpu.csv").open(encoding="utf-8-sig", newline="")))
    names = ("Shader.compile", "physics.gpu.graph_build", "graph.compile", "graph.execute", "graphics.submit", "graphics.wait")
    if not all(any(r["name"] == name for r in rows) for name in names) or not any(r["name"] == "cloth visualization" for r in gpu):
        raise ValueError("incomplete Tracy capture")
    roots = [r for r in rows if r["name"] == "benchmark.solve_batch"]
    if len(roots) != 38:
        raise ValueError("Tracy omitted solve batches")
    measurement = read_measurement(directory / "measurement.json")
    if len(gpu) < sum(b["gpu"]["zones"] for b in measurement["batches"] + measurement["draws"]):
        raise ValueError("Tracy omitted GPU intervals")
    result = {"all": {name: distribution([int(r["exec_time_ns"]) for r in rows if r["name"] == name]) for name in names}}
    for phase, starts in (("cold", {0}), ("steady", set(range(32, 296, 8)))):
        values = {name: [] for name in names[1:]}
        for root in roots:
            if int(root["value"].split()[0]) not in starts:
                continue
            begin = int(root["ns_since_start"])
            end = begin + int(root["exec_time_ns"])
            for name in values:
                values[name].append(sum(int(r["exec_time_ns"]) for r in rows if r["name"] == name
                                       and r["thread"] == root["thread"] and begin <= int(r["ns_since_start"]) < end))
        result[phase] = {name: distribution(v) for name, v in values.items()}
    result["cpu_zones"] = len(rows)
    result["gpu_zones"] = len(gpu)
    return result


def summarize(directory):
    manifest = json.loads((directory / "manifest.json").read_text(encoding="utf-8"))
    if manifest["status"] != "passed" or manifest["repetitions"] < 3:
        raise ValueError("baseline requires three complete repeats")
    reports = {}
    rows = []
    compiler = None
    device = None
    for size in SIZES:
        for backend, modes in (("cpu", ("off",)), ("gpu", MODES)):
            reference = None
            for mode in modes:
                runs = []
                for rep in range(manifest["repetitions"]):
                    path = directory / f"r{rep}-{backend}-{size}-{mode}"
                    m = read_measurement(path / "measurement.json")
                    if m["backend"] != backend or m["size"] != size or m["timestamps"] != (mode != "off") or m["tracy_compiled"] != (mode == "tracy") or m["tracy_connected"] != (mode == "tracy"):
                        raise ValueError("mode/build mismatch")
                    if compiler is not None and compiler != m["compiler"]:
                        raise ValueError("compiler changed between measurements")
                    compiler = m["compiler"]
                    if backend == "gpu":
                        if device is not None and device != m["device"]:
                            raise ValueError("device/driver changed between measurements")
                        device = m["device"]
                    if reference is None:
                        reference = m["particles"]
                    elif reference != m["particles"]:
                        raise ValueError("particle arrays changed across repeats/acquisition modes")
                    steady = [b for b in m["batches"] if b["phase"] == "steady"]
                    r = {"repeat": rep, "cold_batch_ns": m["batches"][0]["wall_ns"],
                         "steady_wall_ns": distribution([b["wall_ns"] for b in steady]), "metrics": m["metrics"]}
                    for key in ("cpu_initialize_ns", "device_queue_ns", "acquisition_setup_ns", "gpu_initialize_ns", "renderer_initialize_ns", "readback_ns", "difference", "device"):
                        if key in m:
                            r[key] = m[key]
                    if backend == "gpu":
                        r["steady_advance_ns"] = distribution([b["advance_ns"] for b in steady])
                        r["steady_wait_ns"] = distribution([b["wait_ns"] for b in steady])
                        r["draw_cold_ns"] = m["draws"][0]["wall_ns"]
                        r["draw_wall_ns"] = distribution([b["wall_ns"] for b in m["draws"][1:]])
                    if mode != "off":
                        r["gpu"] = {key: distribution([b["gpu"][key] for b in steady]) for key in
                                    ("submission_ns", "compute_ns", "transfer_ns", "draw_ns")}
                        r["draw_gpu_ns"] = distribution([b["gpu"]["draw_ns"] for b in m["draws"][1:]])
                    if mode == "tracy":
                        r["tracy"] = cpu_phases(path)
                    runs.append(r)
                key = f"{backend}-{size}-{mode}"
                reports[key] = runs
                medians = [r["steady_wall_ns"]["p50"] for r in runs]
                rows.append({"case": key, "cold_batch_ms": statistics.median(r["cold_batch_ns"] for r in runs) / 1e6,
                             "steady_batch_ms": statistics.median(medians) / 1e6,
                             "steady_p50_range_ms": [min(medians) / 1e6, max(medians) / 1e6]})
    overhead = {}
    for size in SIZES:
        off = reports[f"gpu-{size}-off"]
        overhead[str(size)] = {mode: [100 * (m["steady_wall_ns"]["p50"] / b["steady_wall_ns"]["p50"] - 1)
                                    for m, b in zip(reports[f"gpu-{size}-{mode}"], off)] for mode in MODES[1:]}
    controls = {}
    if manifest["control"]:
        for backend in ("cpu", "gpu"):
            for size in SIZES:
                groups = {}
                for rep in range(manifest["repetitions"]):
                    m = json.loads((directory / f"r{rep}-control-{backend}-{size}/control.json").read_text(encoding="utf-8"))
                    if m["status"] != "passed":
                        raise ValueError("failed control fixture")
                    for s in m["samples"]:
                        key = s["phase"] + ":" + s["method"]
                        groups.setdefault(key, []).append(s)
                controls[f"{backend}-{size}"] = {key: {
                    "latency_ns": distribution([s["latency_ns"] for s in samples]),
                    "overlap_n": sum(s.get("client_intervals_overlap", False) for s in samples),
                    "exit_ns": distribution([s["exit_since_request_ns"] for s in samples]) if "exit_since_request_ns" in samples[0] else None
                } for key, samples in groups.items()}
    summary = {"schema": 1, "status": "passed", "rows": rows, "overhead_percent": overhead, "runs": reports, "controls": controls}
    save(directory / "summary.json", summary)
    lines = ["| Case | Cold batch ms | Steady batch p50 ms | Across-run p50 range ms |",
             "| --- | --- | --- | --- |"]
    for row in rows:
        lo, hi = row["steady_p50_range_ms"]
        lines.append(f"| {row['case']} | {row['cold_batch_ms']:.3f} | {row['steady_batch_ms']:.3f} | {lo:.3f}–{hi:.3f} |")
    (directory / "summary.md").write_text("\n".join(lines) + "\n", encoding="utf-8")
    return summary


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--off-dir", type=Path, default=ROOT / "out/build/windows-graphics")
    parser.add_argument("--tracy-dir", type=Path, default=ROOT / "out/build/windows-graphics-profiling")
    parser.add_argument("--tools-dir", type=Path, default=ROOT / "out/profiling-tools/vcpkg_installed/x64-windows/tools/tracy")
    parser.add_argument("--repetitions", type=int, default=3)
    parser.add_argument("--port", type=int, default=18088)
    parser.add_argument("--skip-control", action="store_true", help="partial measurement only; not M11.2 acceptance")
    parser.add_argument("--summarize", type=Path)
    args = parser.parse_args()
    if args.summarize:
        summarize(args.summarize.resolve())
        return
    if not 3 <= args.repetitions <= 10 or not 1024 <= args.port <= 65535:
        parser.error("repetitions must be 3..10, port 1024..65535")
    bins = {"off": args.off_dir.resolve() / "bin/RelWithDebInfo/dk-simulation-benchmark.exe",
            "tracy": args.tracy_dir.resolve() / "bin/RelWithDebInfo/dk-simulation-benchmark.exe"}
    runner = args.off_dir.resolve() / "bin/RelWithDebInfo/dk-run.exe"
    ctl = args.off_dir.resolve() / "bin/RelWithDebInfo/dk-ctl.exe"
    tools = args.tools_dir.resolve()
    for path in (*bins.values(), runner, ctl, tools / "tracy-capture.exe", tools / "tracy-csvexport.exe"):
        if not path.is_file():
            raise FileNotFoundError(path)
    directory = ROOT / "out/benchmarks" / ("simulation-" + datetime.now().strftime("%Y%m%d-%H%M%S") + "-" + uuid4().hex[:8])
    directory.mkdir(parents=True)
    manifest = {"schema": 1, "status": "failed", "started_at": datetime.now().astimezone().isoformat(),
                "repetitions": args.repetitions, "control": not args.skip_control, "platform": platform.platform(),
                "cpu": platform.processor(), "logical_cpus": os.cpu_count(), "python": platform.python_version(),
                "commit": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip(),
                "working_tree": subprocess.check_output(["git", "status", "--short"], cwd=ROOT, text=True),
                "binaries": {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in (*bins.values(), runner, ctl)},
                "sources": {str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest() for p in (
                    ROOT / "tests/integration/SimulationBenchmark.cpp", ROOT / "tests/integration/CMakeLists.txt",
                    ROOT / "scripts/benchmark-simulation.py", ROOT / "scripts/benchmark-simulation-control.py")},
                "order": []}
    # Preserve build options/toolchain, power policy and exact source changes with raw data.
    for name, build in (("off", args.off_dir), ("tracy", args.tracy_dir)):
        (directory / (name + "-CMakeCache.txt")).write_bytes((build / "CMakeCache.txt").read_bytes())
    run(["git", "diff", "HEAD"], directory, "source-diff")
    run(["powercfg", "/getactivescheme"], directory, "power")
    run(["powershell", "-NoProfile", "-Command", "Get-CimInstance Win32_Processor | Select-Object Name | ConvertTo-Json"], directory, "cpu-model")
    env = dict(os.environ, TRACY_PORT=str(args.port), TRACY_ONLY_LOCALHOST="1", TRACY_ONLY_IPV4="1", TRACY_NO_EXIT="0")
    run([tools / "tracy-csvexport.exe", "--version"], directory, "tracy-version")
    if "0.14.1" not in (directory / "tracy-version.stdout.log").read_text():
        raise RuntimeError("expected Tracy 0.14.1")
    print(directory, flush=True)
    try:
        for rep in range(args.repetitions):
            for size in SIZES[rep % 3:] + SIZES[:rep % 3]:
                cases = [("cpu", "off")] + [("gpu", mode) for mode in MODES[rep % 3:] + MODES[:rep % 3]]
                for backend, mode in cases:
                    name = f"r{rep}-{backend}-{size}-{mode}"
                    path = directory / name
                    path.mkdir()
                    manifest["order"].append(name)
                    command = [bins["tracy" if mode == "tracy" else "off"], "--size", size, "--output", path / "measurement.json"]
                    if backend == "gpu":
                        command.append("--gpu")
                    if mode != "off":
                        command.append("--timestamps")
                    print(name, flush=True)
                    if mode == "tracy":
                        trace_run(command, tools, path, env)
                    else:
                        run(command, path, "probe", env=env)
                    read_measurement(path / "measurement.json")
                    time.sleep(0.2)
        if not args.skip_control:
            spec = importlib.util.spec_from_file_location("control", ROOT / "scripts/benchmark-simulation-control.py")
            control = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(control)
            for rep in range(args.repetitions):
                for size in SIZES[rep % 3:] + SIZES[:rep % 3]:
                    for backend in ("cpu", "gpu"):
                        name = f"r{rep}-control-{backend}-{size}"
                        print(name, flush=True)
                        manifest["order"].append(name)
                        control.measure(runner, ctl, directory / name, size, backend)
        manifest["status"] = "passed"
    except Exception as error:
        manifest["error"] = repr(error)
        raise
    finally:
        manifest["finished_at"] = datetime.now().astimezone().isoformat()
        save(directory / "manifest.json", manifest)
    summarize(directory)
    print("Baseline complete: " + str(directory / "summary.json"), flush=True)


if __name__ == "__main__":
    main()
