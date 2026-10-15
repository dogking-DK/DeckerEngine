"""M16.11 fixed quality gates and three independent performance processes; raw evidence retained."""
import argparse
import hashlib
import json
import math
from pathlib import Path
import statistics
import subprocess
import time
from uuid import uuid4

CASES = ("area-lambert", "ggx-box", "disk-mask")
SEEDS = [101 + 7919*i for i in range(8)]
LEVELS = (64, 256, 1024)
ROOT = Path(__file__).resolve().parents[1]


def need(value, reason):
    if not value:
        raise ValueError(reason)


def linear(value):
    need(len(value) == 16 and all(len(p) == 3 for p in value), "Missing linear pixels")
    need(all(isinstance(v, (float, int)) and math.isfinite(v) and v >= 0 for p in value for v in p), "Invalid linear radiance")
    return value


def mean_image(images):
    for image in images:
        linear(image)
    return [[statistics.mean(i[p][c] for i in images) for c in range(3)] for p in range(16)]


def error(images, reference, indices=range(16)):
    for image in images:
        linear(image)
    indices = list(indices)
    if not indices:
        return None
    squared = sum((i[p][c]-reference[p][c])**2 for i in images for p in indices for c in range(3))
    base = len(images) * max(sum(reference[p][c]**2 for p in indices for c in range(3)), len(indices)*3e-12)
    return math.sqrt(squared/base)


def common(report, kind):
    need(report["schema"] == 1 and report["status"] == "passed" and report["kind"] == kind, "Failed or mislabeled raw run")
    need(not report["cpu_live_allocations"] and not report["vma_live_allocations"], "Live allocations after release")
    need(report["diagnostics"] == dict(errors=0, warnings=0), "GPU diagnostic failure")
    need([v["input"]["name"] for v in report["cases"]] == list(CASES), "Missing/duplicate fixture")


def quality(report):
    common(report, "quality")
    need(report["validation"] and not report["timestamps"] and not report["smoke"], "Quality must require validation")
    results = []
    for case in report["cases"]:
        rows = case["seeds"]
        need([r["seed"] for r in rows] == SEEDS, "Missing/duplicate quality seed")
        need(all((r["cpu_n_spp"], r["cpu_4n_spp"], r["gpu_spp"]) == (16384, 65536, 65536) for r in rows), "Changed reference sample counts")
        n = mean_image([r["cpu_n"] for r in rows])
        reference = mean_image([r["cpu_4n"] for r in rows])
        convergence = error([n], reference)
        squared = sum(sum((a-b)**2 for a, b in zip(x, y)) for r in rows for x, y in zip(linear(r["gpu_4n"]), linear(r["cpu_4n"])))
        base = sum(v*v for r in rows for p in r["cpu_4n"] for v in p)
        agreement = math.sqrt(squared/max(base, 8*16*3e-12))
        need(convergence <= .005, f"{case['input']['name']}: CPU N/4N reference unconverged: {convergence}")
        need(agreement <= .02, f"{case['input']['name']}: CPU/GPU mismatch: {agreement}")
        means = [statistics.mean(sum(p)/3 for p in r["cpu_4n"]) for r in rows]
        half = 2.365*statistics.stdev(means)/math.sqrt(8)
        ladder = []
        for r in rows:
            need([p["completed_spp"] for p in r["progressive"]] == list(LEVELS), "Incorrect progressive sample range")
        for index, spp in enumerate(LEVELS):
            images = [r["progressive"][index]["linear"] for r in rows]
            bright = [p for p in range(16) if sum(reference[p]) > 2*statistics.mean(sum(v) for v in reference)]
            ladder.append(dict(spp=spp, nrmse=error(images, reference), top_roi=error(images, reference, range(8)),
                               bottom_roi=error(images, reference, range(8, 16)), bright_roi=error(images, reference, bright)))
        need(all(b["nrmse"] <= max(a["nrmse"], 1e-6) for a, b in zip(ladder, ladder[1:])), f"Convergence ladder regressed: {ladder}")
        need(all(case["dynamic"].get(k) is True for k in ("reset_fresh", "camera_fresh", "pending_cancel_recovery")), "Dynamic recovery failed")
        results.append(dict(name=case["input"]["name"], reference=reference, n_4n_nrmse=convergence, cpu_gpu_nrmse=agreement,
                            seed_mean=statistics.mean(means), ci95_half_width=half, ladder=ladder,
                            quality_target_spp=next((r["spp"] for r in ladder if r["nrmse"] <= .02), None)))
    return results


def profile(value):
    need(value and value["period_ns"] > 0 and value["valid_bits"] > 0 and value["submission"] > 0, "Missing GPU profile")
    need(value["zones"] and math.isfinite(value["submission_ms"]) and value["submission_ms"] >= 0, "Invalid GPU time")
    need(all(math.isfinite(z["gpu_ms"]) and z["gpu_ms"] >= 0 for z in value["zones"]), "Invalid GPU zone")
    return value["submission_ms"]


def stats(values):
    need(values and all(math.isfinite(v) and v >= 0 for v in values), "Missing/nonfinite measurement")
    ordered = sorted(values)
    return dict(n=len(values), median=statistics.median(values), p95=ordered[math.ceil(.95*len(values))-1], min=min(values), max=max(values))


def performance(report, reference, smoke=False):
    common(report, "performance")
    need(not report["validation"] and report["timestamps"] and report["smoke"] == smoke and not report["tracy_connected"], "Invalid performance mode")
    result = []
    for case, truth in zip(report["cases"], reference):
        need(case["input"]["name"] == truth["name"], "Reference input mismatch")
        width, height, count = (160, 90, 2) if smoke else (1920, 1080, 7)
        need(case["workload"] == dict(width=width, height=height, max_bounces=8, batch_spp=1, tile_edge=64, frames=count), "Altered workload")
        frames = case["frames"]
        need([f["index"] for f in frames] == list(range(count)), "Missing/duplicated frame")
        gpu = []
        for i, frame in enumerate(frames):
            need(frame["phase"] == ("steady" if i else "cold") and frame["completed_spp"] == i+1, "Incorrect frame classification")
            need(len(frame["tiles"]) == math.ceil(width/64)*math.ceil(height/64), "Missing tile measurement")
            ids = [t["gpu"]["submission"] for t in frame["tiles"]]
            need(len(ids) == len(set(ids)), "Duplicated tile submission")
            gpu.append(sum(profile(t["gpu"]) for t in frame["tiles"]))
            need(frame["memory"]["current"] <= frame["memory"]["peak"] <= frame["memory"]["limit"] == 2**30, "Budget violation")
        for name in ("tone", "linear_readback"):
            profile(case[name]["gpu"])
        profile(case["upload_build_gpu"])
        ladder = case["quality_ladder"]
        need([v["seed"] for v in ladder] == SEEDS, "Missing performance quality seed")
        levels = []
        for row in ladder:
            need([v["spp"] for v in row["levels"]] == list(LEVELS), "Missing performance quality level")
        for i, spp in enumerate(LEVELS):
            levels.append(dict(spp=spp, nrmse=error([r["levels"][i]["linear"] for r in ladder], truth["reference"]),
                               population_wall_ms=sum(r["levels"][i]["cumulative_ms"] for r in ladder)))
        reached = next((v for v in levels if v["nrmse"] <= .02), None)
        result.append(dict(name=truth["name"], cold_wall_ms=frames[0]["wall_ms"], cold_gpu_ms=gpu[0],
                           steady_wall_ms=stats([f["wall_ms"] for f in frames[1:]]), steady_gpu_ms=stats(gpu[1:]),
                           cpu_record_submit_ms=stats([sum(t["record_submit_ms"] for t in f["tiles"]) for f in frames[1:]]),
                           cpu_wait_poll_ms=stats([sum(t["wait_poll_ms"] for t in f["tiles"]) for f in frames[1:]]),
                           memory_steady=case["memory_steady"], memory_inflight_with_old=case["memory_inflight_with_old"],
                           cancel_call_ms=case["cancel_call_ms"], cancel_reclaim_ms=case["cancel_reclaim_ms"],
                           tone=case["tone"], readback=case["linear_readback"], quality_ladder=levels,
                           quality_reached=reached, realtime_candidate_met=max(gpu[1:]) <= 16.67 and max(f["wall_ms"] for f in frames[1:]) <= 16.67))
    return result


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def source_hashes():
    paths = list((ROOT/"engine/render/pathtracing").rglob("*")) + list((ROOT/"tests/fixtures/render-disk").rglob("*"))
    paths += [ROOT/"tests/integration/PathBenchmark.cpp", ROOT/"tests/integration/PathBenchmarkSupport.hpp", Path(__file__)]
    return {p.relative_to(ROOT).as_posix(): digest(p) for p in sorted(paths) if p.is_file()}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--executable", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--quality-only", action="store_true")
    parser.add_argument("--quality-report", type=Path)
    parser.add_argument("--smoke", action="store_true")
    args = parser.parse_args()
    directory = args.output / (time.strftime("%Y%m%d-%H%M%S") + "-" + uuid4().hex[:8])
    directory.mkdir(parents=True)
    manifest = dict(schema=1, status="running", binary_sha256=digest(args.executable), sources=source_hashes(), runs=[])
    def save():
        (directory/"manifest.json").write_text(json.dumps(manifest, indent=2), encoding="utf-8")
    def launch(name, options):
        raw = directory/(name+".json")
        row = dict(name=name, status="running", raw=raw.name)
        manifest["runs"].append(row); save()
        with (directory/(name+".stdout.log")).open("wb") as out, (directory/(name+".stderr.log")).open("wb") as err:
            process = subprocess.run([str(args.executable), *options, "--output", str(raw)], stdout=out, stderr=err,
                                     timeout=900, creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
        need(process.returncode == 0, f"{name} failed; inspect {raw}")
        row.update(status="collected", sha256=digest(raw)); save()
        return json.loads(raw.read_text())
    try:
        if args.quality_report:
            original = json.loads(args.quality_report.read_text())
            prior = json.loads((args.quality_report.parent/"manifest.json").read_text())
            need(prior["status"] == "passed" and prior["binary_sha256"] == manifest["binary_sha256"] and prior["sources"] == manifest["sources"], "Stale quality source/binary")
            source_row = next((r for r in prior["runs"] if r["raw"] == args.quality_report.name), None)
            need(source_row and source_row["status"] == "passed" and source_row["sha256"] == digest(args.quality_report), "Modified quality evidence")
            (directory/"quality.json").write_bytes(args.quality_report.read_bytes())
            manifest["runs"].append(dict(name="quality", status="passed", raw="quality.json", sha256=digest(args.quality_report), reused=True))
        else:
            original = launch("quality", ["--quality"])
        reference = quality(original)
        summary = dict(schema=1, status="running", quality=reference, rounds=[])
        if not args.quality_only:
            for i in range(1, 4):
                data = launch(f"round-{i}", ["--smoke"] if args.smoke else [])
                need(data["device"] == original["device"] and [c["input"] for c in data["cases"]] == [c["input"] for c in original["cases"]], "Changed device/input")
                summary["rounds"].append(dict(round=i, device_queue_ms=data["device_queue_ms"], compile_pipeline_ms=data["compile_pipeline_ms"], cases=performance(data, reference, args.smoke)))
        need(manifest["binary_sha256"] == digest(args.executable) and manifest["sources"] == source_hashes(), "Source or binary changed during run")
        summary["status"] = manifest["status"] = "passed"
        (directory/"summary.json").write_text(json.dumps(summary, indent=2), encoding="utf-8")
        for r in manifest["runs"]:
            r["status"] = "passed"
    except Exception as error:
        manifest.update(status="failed", error=str(error))
        raise
    finally:
        save(); print(directory, flush=True)


if __name__ == "__main__":
    main()
