"""Release OpenVDB conversion and real-GPU file benchmarks; independent processes per round."""
import argparse
import ctypes
import hashlib
import json
import os
from pathlib import Path
import statistics
import subprocess
import time


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


class Counters(ctypes.Structure):
    _fields_ = [("cb", ctypes.c_ulong), ("faults", ctypes.c_ulong)] + [
        (name, ctypes.c_size_t) for name in ("peak_working", "working", "peak_paged", "paged",
                                           "peak_nonpaged", "nonpaged", "commit", "peak_commit", "private")]


def run(command, prefix):
    prefix.parent.mkdir(parents=True, exist_ok=True)
    start = time.perf_counter()
    peak = dict(working=0, private=0, process_peak_working=0, process_peak_commit=0, samples=0)
    with prefix.with_suffix(".stdout.log").open("wb") as out, prefix.with_suffix(".stderr.log").open("wb") as err:
        process = subprocess.Popen([str(x) for x in command], stdout=out, stderr=err,
                                   creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0)
        while process.poll() is None:
            if os.name == "nt":
                c = Counters(); c.cb = ctypes.sizeof(c)
                ok = ctypes.windll.psapi.GetProcessMemoryInfo(ctypes.c_void_p(int(process._handle)), ctypes.byref(c), c.cb)
                if ok:
                    peak["samples"] += 1
                    for key, field in (("working", "working"), ("private", "private"),
                                       ("process_peak_working", "peak_working"), ("process_peak_commit", "peak_commit")):
                        peak[key] = max(peak[key], getattr(c, field))
            time.sleep(.001)
        if process.returncode:
            raise RuntimeError(f"Exit {process.returncode}: {prefix.with_suffix('.stderr.log').read_text(errors='replace')}")
    return dict(wall_ms=(time.perf_counter()-start)*1000, process_memory=peak,
                memory_scope="Whole process; sampled peaks are lower bounds; internal scratch is not isolated")


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--assetc", required=True, type=Path)
    p.add_argument("--benchmark", type=Path)
    p.add_argument("--project-root", type=Path, default=Path.cwd())
    p.add_argument("--source", default="projects/demo/assets/gltf/Sponza/glTF/Sponza.gltf")
    p.add_argument("--unit-scale", type=float, default=.008)
    p.add_argument("--output", type=Path, required=True, help="New directory inside project root")
    args = p.parse_args()
    root = args.project_root.resolve(); output = args.output.resolve(); output.relative_to(root)
    output.mkdir(parents=True, exist_ok=False)
    report = dict(status="running", source=args.source, unit_scale=args.unit_scale, rounds=[],
                  assetc_sha256=digest(args.assetc), configuration="Release required; verify executable build before running")
    source = root / args.source
    gltf = json.loads(source.read_text(encoding="utf-8"))
    dependencies = [source] + [source.parent / item["uri"] for key in ("images", "buffers")
                              for item in gltf.get(key, []) if "uri" in item]
    report["source_sha256"] = {str(f.relative_to(root)): digest(f) for f in dependencies}
    if args.benchmark:
        report["benchmark_sha256"] = digest(args.benchmark)
    def save():
        (output / "report.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
    save()
    try:
        for resolution in (64, 128, 256):
            for round_id in range(3):
                name = f"sponza-{resolution}-r{round_id}"
                bundle = output / name
                print(f"Converting {name}", flush=True)
                process = run([args.assetc, "voxelize", "--project-root", root, "--source", args.source,
                               "--output", bundle.relative_to(root), "--resolution", resolution,
                               "--unit-scale", args.unit_scale], output / (name + "-convert"))
                row = dict(resolution=resolution, round=round_id, process=process,
                           conversion=json.loads((bundle / "conversion.json").read_text()))
                report["rounds"].append(row); save()
                if args.benchmark:
                    if resolution == 128 and round_id == 0:
                        run([args.benchmark, "--asset", bundle, "--smoke", "--validation", "--output",
                             output / "validation/report.json"], output / "validation/run")
                        validation = json.loads((output / "validation/report.json").read_text())
                        assert validation["status"] == "passed"
                        report["validation"] = "validation/report.json"
                    print(f"GPU benchmarking {name}", flush=True)
                    row["gpu_process"] = run([args.benchmark, "--asset", bundle, "--round", round_id, "--output",
                                              output / (name + "-gpu") / "report.json"], output / (name + "-gpu") / "run")
                    gpu = json.loads((output / (name + "-gpu") / "report.json").read_text())
                    assert gpu["status"] == "passed" and gpu["configuration"] == "Release"
                    row["gpu_report"] = name + "-gpu/report.json"; save()
        assert all(digest(root / path) == sha for path, sha in report["source_sha256"].items())
        report["summary"] = []
        for resolution in (64, 128, 256):
            rows = [r for r in report["rounds"] if r["resolution"] == resolution]
            summary = dict(resolution=resolution)
            for key in ("distance_ms", "bake_ms", "total_ms", "elapsed_ms", "compile_nano_ms", "nano_bytes", "occupied"):
                values = [r["conversion"][key] for r in rows]
                summary[key] = dict(mean=statistics.mean(values), min=min(values), max=max(values))
            report["summary"].append(summary)
        report["status"] = "passed"
    except Exception as error:
        report["status"] = "failed"; report["error"] = str(error); raise
    finally:
        save()
    print(output / "report.json", flush=True)


if __name__ == "__main__":
    main()
