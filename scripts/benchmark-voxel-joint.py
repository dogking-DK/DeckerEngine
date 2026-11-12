"""M23.13 joint baseline: strict evidence validation, serial runs, reproducible aggregation."""
from __future__ import annotations
import argparse
from datetime import datetime, timezone
import gzip
import hashlib
import json
import math
import os
from pathlib import Path
import statistics
import subprocess

EDGES = (8, 32, 128)
CASES = tuple(f"{kind}{size}" for size in (64, 128, 256) for kind in ("empty", "sparse", "solid")) + ("islands16k", "root_tiles", "checker64")
SMOKE = tuple(c for c in CASES if c.endswith("64") or c.startswith("sparse") or c in ("solid256", "islands16k", "root_tiles"))
ROOT = Path(__file__).resolve().parents[1]

def require(value, message):
    if not value: raise ValueError(message)

def finite(value, name):
    require(isinstance(value, (int, float)) and not isinstance(value, bool) and math.isfinite(value) and value >= 0, "invalid " + name)
    return value

def profile(p):
    finite(p["submission_ms"], "GPU interval")
    require(p["valid_bits"] > 0 and finite(p["period_ns"], "period") > 0 and p["zones"], "missing timestamp protocol")
    for zone in p["zones"]:
        require(zone["name"], "unnamed zone"); finite(zone["ms"], "zone")

def memory(m):
    for key in ("allocations", "allocation_bytes", "block_bytes", "device_local_bytes", "host_visible_bytes"):
        finite(m[key], key)
    require(m["allocation_bytes"] <= m["block_bytes"], "allocation exceeds VMA blocks")
    if "owned" in m:
        o = m["owned"]
        require(0 <= o["current"] <= o["peak"] <= o["limit"], "invalid owned memory")
        finite(m["sbt_region_bytes"], "SBT regions")

def view(v, path, samples):
    frames = v["frames"]
    require(len(frames) == samples + 3, "missing frame samples")
    for i, f in enumerate(frames):
        require(f["phase"] == ("cold" if i == 0 else "warmup" if i < 3 else "steady"), "frame phase mismatch")
        for name in ("cpu_submit_ms", "wait_ms", "end_to_end_ms"): finite(f[name], name)
        if path:
            require(f["spp"] == i + 1 and f["tiles"], "wrong sample interval")
            for tile in f["tiles"]:
                profile(tile["gpu"]); finite(tile["record_submit_ms"], "record"); finite(tile["wait_poll_ms"], "wait")
            require(math.isclose(f["gpu_ms"], sum(t["gpu"]["submission_ms"] for t in f["tiles"]), rel_tol=1e-10, abs_tol=1e-10), "GPU sum mismatch")
        else:
            profile(f["gpu"]); finite(f["readback_ms"], "readback")
    if path:
        require(v["linear"]["pixels"] == v["width"] * v["height"], "missing linear pixels")
        finite(v["linear"]["max_layout_error"], "layout error")
        require(finite(v["linear"]["max_scaled_layout_error"], "scaled layout error") <= 2e-4, "layout quality failure")
        profile(v["tone"]["gpu"]); profile(v["linear"]["gpu"])
        for name in ("memory_steady", "memory_old_and_pending", "memory_released_view"): memory(v[name])
        require(v["memory_old_and_pending"]["owned"]["current"] >= v["memory_steady"]["owned"]["current"], "pending memory lost old frame")
    else:
        require(v["nano_gpu_payload_bytes"] > 0 and v["palette_payload_bytes"] > 0, "missing raster payload")
        if v["quality"].get("pixels", 0):
            require(v["quality"]["maximum_color_error_bytes"] <= 1 and v["quality"]["maximum_depth_error"] <= 2e-6, "raster quality failure")

def statistics_quality(rows):
    require(len(rows) == 6 and {(r["model"], r["edge"]) for r in rows} == {(m, e) for m in ("lambert", "ggx") for e in EDGES}, "missing statistical model/layout")
    result = []
    for row in rows:
        seeds = row["seeds"]
        require(len(seeds) == 8 and [s["seed"] for s in seeds] == [101 + 7919*i for i in range(8)], "statistical seed mismatch")
        for s in seeds:
            require(s["spp"] == 4096 and len(s["rays"]) == 2, "statistical ray/spp mismatch")
            for r in s["rays"]:
                for key in ("cpu", "cpu_second", "native", "native_second", "triangle"):
                    require(len(r[key]) == 3, "RGB size")
                    for x in r[key]: finite(x, "statistical RGB")
                for key in ("native", "triangle"):
                    require(max(abs(a-b) for a, b in zip(r[key], r["cpu"])) <= .006, "statistical mean failure")
                require(max(abs(a-b) for a, b in zip(r["native_second"], r["cpu_second"])) <= .008, "statistical moment failure")
        for ray in range(2):
            item = dict(model=row["model"], edge=row["edge"], ray=ray)
            for key in ("cpu", "native", "triangle"):
                columns = [[s["rays"][ray][key][channel] for s in seeds] for channel in range(3)]
                item[key] = dict(mean=[statistics.mean(c) for c in columns], ci95_half=[2.364624251*statistics.stdev(c)/math.sqrt(8) for c in columns])
            item["maximum_seed_error"] = max(abs(s["rays"][ray]["native"][channel]-s["rays"][ray]["cpu"][channel]) for s in seeds for channel in range(3))
            result.append(item)
    return result

def validate_run(d, formal=True):
    require((d["schema"], d["kind"], d["matrix_version"], d["measurement_version"]) == (1, "voxel-joint-baseline", 2, 1), "unknown joint protocol")
    require(d["status"] == "passed" and not d["tracy_connected"], "failed/Tracy run")
    require(d["diagnostics"] == dict(errors=0, warnings=0) and d["heap_live_allocations"] == 0, "diagnostics/lifetime failure")
    memory(d["device_only"]); memory(d["after_release"])
    require(d["after_release"]["allocations"] == d["device_only"]["allocations"], "GPU leak")
    require(d["round"] in (0, 1, 2) and isinstance(d["shader_fingerprint"], int), "missing round/shader source")
    require(d["warmup"] == 2 and d["seed"] == 2313 and d["max_bounces"] == 4, "sampling protocol mismatch")
    if formal:
        require(d["configuration"] == "Release" and not d["validation"] and not d["smoke"], "performance requires full Release without validation")
        finite(d["program_create_ms"], "program creation")
    if d["smoke"]:
        require(d["validation"], "quality requires validation")
        statistics_quality(d["statistics"])
    else: require(d["statistics"] is None, "performance mixed with CPU quality")
    samples = 2 if d["smoke"] else 8
    require(d["steady_samples"] == samples, "steady sample count")
    view(d["precondition_raster"], False, 0 if d["smoke"] else 61)
    view(d["precondition_path"], True, 0 if d["smoke"] else 29)
    require(d["precision_rejection"]["span"] == 131072 and "precision" in d["precision_rejection"]["reason"].lower() or
            d["precision_rejection"]["span"] == 131072 and "offset" in d["precision_rejection"]["reason"].lower(), "unproven precision boundary")
    expected = SMOKE if d["smoke"] else CASES
    require(len(d["cases"]) == len(expected) and {c["name"] for c in d["cases"]} == set(expected), "missing/duplicate inputs")
    sizes = ((640, 480),) if d["smoke"] else ((640, 480), (1280, 720))
    views = {(w, h, p) for w, h in sizes for p in ("perspective", "orthographic")}
    for c in d["cases"]:
        for key in ("source_build_ms", "conversion_ms"): finite(c[key], key)
        st = c["storage"]
        require(st["nano_bytes"] > 0 and len(st["nodes"]) == len(st["tiles"]) == 3, "missing sparse storage")
        for key in ("source_tree_bytes", "normalized_tree_bytes", "active_voxels", "content_hash"): finite(st[key], key)
        m = c["conversion_process_memory"]
        require(m["samples"] > 0 and m["interval_ms"] == 1 and not m["internal_scratch_exact"], "unproven conversion peak")
        require(m["sampled_working_peak"] >= m["working_before"] and m["sampled_private_peak"] >= m["private_before"], "invalid process peak")
        memory(c["before"]); memory(c["after_release"])
        require(all(c["before"][k] == c["after_release"][k] for k in ("allocations", "allocation_bytes")), "case leak")
        profile(c["raster_upload"]["gpu"])
        require(len(c["raster"]) == len(views) and {(v["width"], v["height"], v["projection"]) for v in c["raster"]} == views, "missing raster view")
        for v in c["raster"]: view(v, False, samples)
        require([r["edge"] for r in c["layouts"]] == list(EDGES), "missing/duplicate/reordered layouts")
        for layout in c["layouts"]:
            finite(layout["cpu_scene_ms"], "CPU scene"); finite(layout["prepare"]["cpu_submit_ms"], "prepare submit"); finite(layout["prepare"]["wall_ms"], "prepare wall")
            geometry = layout["geometry"]
            for value in geometry.values(): finite(value, "geometry memory")
            require(geometry["aabb_input"] == layout["block_count"] * 24, "AABB payload mismatch")
            if layout["block_count"]:
                require(geometry["nano"] == st["nano_bytes"] and geometry["blocks"] == 32*layout["block_count"] and geometry["blas_storage"] > 0 and geometry["tlas_storage"] > 0, "missing geometry accounting")
            if layout["prepare"]["gpu"] is not None:
                require(not layout["geometry_reused"], "cached build reported as new GPU work"); profile(layout["prepare"]["gpu"])
            else: require(layout["block_count"] == 0 or layout["geometry_reused"], "missing AS build timestamp")
            primary = layout["primary"]
            require(primary["rays"] == 1024 and primary["oracle"] in ("dense_dda", "analytic_boxes"), "missing independent primary oracle")
            require(0 <= primary["hits"] <= 1024, "hit count")
            memory(layout["memory_build_with_old"]); memory(layout["memory_scene"])
            require(len(layout["views"]) == len(views) and {(v["width"], v["height"], v["projection"]) for v in layout["views"]} == views, "missing path view")
            for v in layout["views"]: view(v, True, samples)
            if d["smoke"] and (c["name"].startswith("sparse") or c["name"] == "solid256"):
                regression = layout["regression"]
                require(len(regression) == 2 and [(v["width"], v["height"], v["projection"]) for v in regression] == [(640, 480, "perspective"), (1280, 720, "perspective")], "missing grazing-ray regression")
                for v in regression: view(v, True, 8)
    return d

def read(path):
    data = Path(path).read_bytes()
    return json.loads(gzip.decompress(data) if str(path).endswith(".gz") else data)

def stats(values):
    values = sorted(values)
    return dict(n=len(values), median=statistics.median(values), p95=values[math.ceil(.95*len(values))-1], minimum=values[0], maximum=values[-1])

def aggregate(runs, quality):
    require(len(runs) == 3 and sorted(d["round"] for d in runs) == [0, 1, 2], "need three unique rounds")
    validate_run(quality, False)
    require(quality["smoke"] and quality["validation"], "required quality evidence missing")
    for d in runs: validate_run(d)
    identity = lambda d: (d["device"], d["compiler"], d["shader_fingerprint"], d["matrix_version"])
    require(all(identity(d) == identity(runs[0]) for d in runs + [quality]), "mixed device/build/shader protocol")
    require(all(d.get("source_sha256") and d.get("executable_sha256") for d in runs + [quality]), "missing executable/source identity")
    require(len({d["source_sha256"] for d in runs + [quality]}) == len({d["executable_sha256"] for d in runs}) == 1, "mixed measured sources")
    summary = dict(schema=1, kind="voxel-joint-summary", device=runs[0]["device"], shader_fingerprint=runs[0]["shader_fingerprint"], rows=[], cases=[], statistics=statistics_quality(quality["statistics"]))
    for name in CASES:
        cases = [next(c for c in d["cases"] if c["name"] == name) for d in runs]
        require(all(c["storage"] == cases[0]["storage"] and c["domain_min"] == cases[0]["domain_min"] and c["domain_max"] == cases[0]["domain_max"] for c in cases), "unstable input storage")
        case = dict(name=name, storage=cases[0]["storage"], conversion_ms=stats([c["conversion_ms"] for c in cases]), layouts=[])
        for edge in EDGES:
            ls = [next(x for x in c["layouts"] if x["edge"] == edge) for c in cases]
            require(all(l["block_count"] == ls[0]["block_count"] and l["geometry"] == ls[0]["geometry"] for l in ls), "unstable geometry payload")
            case["layouts"].append(dict(edge=edge, block_count=ls[0]["block_count"], geometry=ls[0]["geometry"], prepare_ms=stats([l["prepare"]["wall_ms"] for l in ls]),
                build_reused=[l["geometry_reused"] for l in ls], build_gpu_ms=stats([l["prepare"]["gpu"]["submission_ms"] for l in ls if l["prepare"]["gpu"] is not None]) if any(l["prepare"]["gpu"] is not None for l in ls) else None))
        summary["cases"].append(case)
        for width, height in ((640, 480), (1280, 720)):
            for projection in ("perspective", "orthographic"):
                for edge in (0,) + EDGES:
                    matches = []
                    for c in cases:
                        source = c["raster"] if edge == 0 else next(l["views"] for l in c["layouts"] if l["edge"] == edge)
                        matches.append(next(v for v in source if (v["width"], v["height"], v["projection"]) == (width, height, projection)))
                    frames = [f for v in matches for f in v["frames"] if f["phase"] == "steady"]
                    row = dict(name=name, width=width, height=height, projection=projection, backend="raster" if edge == 0 else "native_path", edge=edge)
                    row["gpu_ms"] = stats([f["gpu"]["submission_ms"] if edge == 0 else f["gpu_ms"] for f in frames])
                    row["cpu_submit_ms"] = stats([f["cpu_submit_ms"] for f in frames]); row["wall_ms"] = stats([f["end_to_end_ms"] for f in frames])
                    if edge:
                        row["inflight_owned_peak"] = max(v["memory_old_and_pending"]["owned"]["current"] for v in matches)
                        row["maximum_layout_error"] = max(v["linear"]["max_layout_error"] for v in matches)
                    summary["rows"].append(row)
    return summary

def source_manifest():
    files = {}
    for root in ("engine", "cmake", "shaders"):
        for p in (ROOT/root).rglob("*"):
            if p.is_file() and p.suffix in (".cpp", ".hpp", ".h", ".inl", ".slang", ".cmake", ".txt"):
                files[p.relative_to(ROOT).as_posix()] = hashlib.sha256(p.read_bytes()).hexdigest()
    for name in ("CMakeLists.txt", "vcpkg.json", "vcpkg-configuration.json", "tests/integration/CMakeLists.txt", "tests/integration/VoxelJointBenchmark.cpp", "tests/integration/VoxelBenchmarkCases.hpp", "tests/integration/VoxelBenchmarkSupport.hpp", "tests/PathVoxelSupport.hpp", "scripts/benchmark-voxel-joint.py"):
        p = ROOT/name
        if p.exists(): files[name] = hashlib.sha256(p.read_bytes()).hexdigest()
    return files

def save_summary(directory, summary):
    directory.mkdir(parents=True, exist_ok=True)
    (directory/"summary.json").write_text(json.dumps(summary, indent=2), encoding="utf-8")
    lines = ["# Joint voxel baseline", "", "GPU raster submissions and native path 1-spp epochs (4 bounces) are distinct workloads; not a shading speedup claim.", "", "| Input | Extent | Projection | Backend / edge | GPU median / p95 ms | CPU submit median ms | Wall median ms |", "| --- | --- | --- | --- | --- | --- | --- |"]
    for r in summary["rows"]:
        lines.append(f"| {r['name']} | {r['width']}x{r['height']} | {r['projection']} | {r['backend']} / {r['edge']} | {r['gpu_ms']['median']:.3f} / {r['gpu_ms']['p95']:.3f} | {r['cpu_submit_ms']['median']:.3f} | {r['wall_ms']['median']:.3f} |")
    (directory/"summary.md").write_text("\n".join(lines)+"\n", encoding="utf-8")

def save_plot(directory, summary):
    """Optional publication artifact; collection and aggregation use only stdlib."""
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    fig, axes = plt.subplots(3, 1, figsize=(13, 11), constrained_layout=True)
    names = [c["name"] for c in summary["cases"]]
    for axis, projection in zip(axes[:2], ("perspective", "orthographic")):
        for edge, color in ((32, "#326ba8"), (128, "#c25c30")):
            ratios = []
            for name in names:
                rows = [r for r in summary["rows"] if r["name"] == name and r["width"] == 1280 and r["projection"] == projection and r["backend"] == "native_path"]
                base = next(r["gpu_ms"]["median"] for r in rows if r["edge"] == 8)
                ratios.append(next(r["gpu_ms"]["median"] for r in rows if r["edge"] == edge) / base)
            axis.plot(names, ratios, "o-", color=color, label=f"minimum edge {edge}")
        axis.axhline(1, color="#555555", linestyle="--", linewidth=1)
        axis.set_title(f"1280 x 720 {projection}: native path GPU median / default edge 8 (lower is faster)")
        axis.set_ylabel("Time ratio"); axis.legend(ncol=2); axis.grid(axis="y", alpha=.2)
    for edge, color in ((8, "#71896b"), (32, "#326ba8"), (128, "#c25c30")):
        values = [next(l["geometry"]["blas_storage"]+l["geometry"]["tlas_storage"] for l in c["layouts"] if l["edge"] == edge)/1024 for c in summary["cases"]]
        axes[2].plot(names, values, "o-", color=color, label=f"minimum edge {edge}")
    axes[2].set_yscale("symlog", linthresh=1)
    axes[2].set_title("Logical BLAS + TLAS storage requirements; excludes scratch, SBT and frame memory")
    axes[2].set_ylabel("KiB (symlog)"); axes[2].legend(ncol=3); axes[2].grid(axis="y", alpha=.2)
    for axis in axes: axis.tick_params(axis="x", labelrotation=25)
    fig.suptitle("M23.13 joint voxel baseline - 3 rounds, 24 steady samples per view", fontsize=15)
    fig.savefig(directory/"baseline.svg"); fig.savefig(directory/"baseline.png", dpi=140); plt.close(fig)

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path); parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--quality", type=Path, required=True); parser.add_argument("--aggregate", type=Path, nargs=3)
    parser.add_argument("--run-quality", action="store_true", help="Run required-validation quality using the same executable and source snapshot")
    parser.add_argument("--plot", action="store_true", help="Also export SVG/PNG charts (requires matplotlib)")
    args = parser.parse_args(); args.output.mkdir(parents=True, exist_ok=True)
    if args.aggregate:
        require(not args.run_quality, "--run-quality cannot combine with --aggregate")
        quality = read(args.quality); validate_run(quality, False)
        runs = [read(p) for p in args.aggregate]
    else:
        require(args.exe is not None and args.exe.is_file(), "--exe required")
        files = source_manifest(); digest = hashlib.sha256(json.dumps(files, sort_keys=True).encode()).hexdigest()
        binary = hashlib.sha256(args.exe.read_bytes()).hexdigest(); runs = []
        env = dict(os.environ, DISABLE_LAYER_AMD_SWITCHABLE_GRAPHICS_1="1")
        if args.run_quality:
            args.quality.parent.mkdir(parents=True, exist_ok=True)
            command = [str(args.exe.resolve()), "--smoke", "--validation", "--output", str(args.quality.resolve())]
            quality_env = dict(env, VK_LAYER_VALIDATE_SYNC="1", VK_VALIDATION_VALIDATE_SYNC="1")
            with (args.output/"quality.log").open("wb") as log:
                subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=True, timeout=1800, env=quality_env)
            require(source_manifest() == files and hashlib.sha256(args.exe.read_bytes()).hexdigest() == binary, "source/binary changed during quality run")
            quality = read(args.quality); validate_run(quality, False)
            quality.update(source_sha256=digest, executable_sha256=binary)
            args.quality.write_text(json.dumps(quality, indent=2), encoding="utf-8")
            print("Completed required-validation quality", flush=True)
        else:
            quality = read(args.quality); validate_run(quality, False)
            require(quality.get("source_sha256") == digest, "quality has a different or unrecorded source; use --run-quality")
        manifest = dict(source_files=files, source_sha256=digest, executable_sha256=binary,
            base_commit=subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip(), quality_sha256=hashlib.sha256(args.quality.read_bytes()).hexdigest(), runs=[])
        env = dict(os.environ, DISABLE_LAYER_AMD_SWITCHABLE_GRAPHICS_1="1")
        for i in range(3):
            output = args.output/f"round-{i}.json"
            command = [str(args.exe.resolve()), "--round", str(i), "--output", str(output.resolve())]
            started = datetime.now(timezone.utc).isoformat()
            with (args.output/f"round-{i}.log").open("wb") as log:
                subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=True, timeout=3600, env=env)
            require(source_manifest() == files and hashlib.sha256(args.exe.read_bytes()).hexdigest() == binary, "source/binary changed during capture")
            data = read(output); validate_run(data)
            data.update(source_sha256=digest, executable_sha256=binary)
            output.write_text(json.dumps(data, indent=2), encoding="utf-8"); runs.append(data)
            manifest["runs"].append(dict(round=i, command=command, started_at=started, finished_at=datetime.now(timezone.utc).isoformat(), sha256=hashlib.sha256(output.read_bytes()).hexdigest()))
            (args.output/"manifest.json").write_text(json.dumps(manifest, indent=2), encoding="utf-8")
            print("Completed joint round", i, flush=True)
    summary = aggregate(runs, quality); save_summary(args.output, summary)
    if args.plot: save_plot(args.output, summary)
    print("Validated three rounds:", args.output)

if __name__ == "__main__": main()
