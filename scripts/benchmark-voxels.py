"""M23.7: run three serial Release rounds, validate raw evidence, and aggregate.

GPU interval and CPU wall time are distinct. VMA snapshots are observed retained
allocations; process samples are lower bounds, not exact conversion scratch peaks.
"""
from __future__ import annotations

import argparse
import gzip
import hashlib
import json
import math
import os
from pathlib import Path
import statistics
import subprocess

CASES = {f"{kind}{size}" for size in (64, 128, 256) for kind in ("empty", "sparse", "solid")} | {"islands", "root_tiles", "checker64"}
SMOKE_CASES = {"empty64", "sparse64", "solid64", "islands", "root_tiles", "checker64"}


def read_run(path):
    data = path.read_bytes()
    return json.loads(gzip.decompress(data) if path.suffix == ".gz" else data)


def require(value, message):
    if not value:
        raise ValueError(message)


def finite(value, name):
    require(isinstance(value, (int, float)) and not isinstance(value, bool) and math.isfinite(value) and value >= 0, f"invalid {name}")
    return value


def profile(p):
    finite(p["submission_ms"], "GPU submission")
    require(p["valid_bits"] > 0 and finite(p["period_ns"], "timestamp period") > 0 and p["zones"], "missing timestamps")
    for z in p["zones"]:
        require(z["name"], "unnamed GPU interval")
        finite(z["ms"], "GPU zone")


def validate_run(d, formal=True):
    require(d["schema"] == 1 and d["matrix_version"] == 1 and d["kind"] == "voxel-raster-baseline", "unknown schema/matrix")
    require(d["status"] == "passed", "incomplete or failed run")
    require(d["diagnostics"] == {"errors": 0, "warnings": 0} and d["heap_live_allocations"] == 0, "validation/lifetime failure")
    require(d["after_release"]["allocations"] == d["device_only"]["allocations"], "GPU leak")
    require(not d["tracy_connected"], "Tracy acquisition changes timing")
    if formal:
        require(d["configuration"] == "Release" and not d["validation"] and not d["smoke"], "formal performance requires full Release without validation")
    measurement = d.get("measurement_version", 1)
    require(measurement in (1, 2), "unknown measurement protocol")
    if measurement == 2 and not d["smoke"]:
        require(d["device_precondition_frames"] == 64 and len(d["precondition"]["frames"]) == 64, "missing device preconditioning")
        for frame in d["precondition"]["frames"]:
            profile(frame["gpu"])
    samples = 2 if d["smoke"] else 8
    require(d["steady_samples"] == samples and d["warmup"] == 2, "sample count mismatch")
    expected = SMOKE_CASES if d["smoke"] else CASES
    require(len(d["cases"]) == len(expected) and {c["name"] for c in d["cases"]} == expected, "missing/duplicate matrix case")
    extent = [(640, 480)] if d["smoke"] else [(640, 480), (1280, 720)]
    expected_views = {(w, h, p) for w, h in extent for p in ("perspective", "orthographic")}
    for c in d["cases"]:
        for name in ("source_build_ms", "conversion_ms"):
            finite(c[name], name)
        s = c["storage"]
        require(s["nano_bytes"] > 0 and s["conversion_live_payload_bytes"] == sum(s[k] for k in ("source_tree_bytes", "normalized_tree_bytes", "nano_bytes")), "incorrect conversion payload")
        require(len(s["nodes"]) == 3 and len(s["tiles"]) == 3 and 0 <= s["occupancy"] <= 1, "missing hierarchy data")
        require(s["aabb_as_sbt"] is None, "RT memory cannot be attributed to raster")
        m = c["conversion_process_memory"]
        require(m["samples"] > 0 and m["interval_ms"] == 1 and not m["internal_scratch_exact"], "missing/laundered process memory samples")
        require(m["sampled_working_peak"] >= m["working_before"] and m["sampled_private_peak"] >= m["private_before"], "invalid phase peak")
        require(m["process_lifetime_working_peak"] >= m["sampled_working_peak"] and m["process_lifetime_commit_peak"] >= m["sampled_private_peak"], "missing/invalid OS lifetime peaks")
        for point in ("upload_pending", "old_generation_and_upload", "before_case", "after_release"):
            for key in ("allocations", "allocation_bytes", "block_bytes", "device_local_bytes", "host_visible_bytes"):
                finite(c[point][key], key)
        require(c["after_release"]["allocations"] == c["before_case"]["allocations"] and c["after_release"]["allocation_bytes"] == c["before_case"]["allocation_bytes"], "case failed to retire")
        profile(c["upload"]["gpu"])
        finite(c["upload"]["cpu_submit_ms"], "upload CPU submit")
        finite(c["upload"]["wait_ms"], "upload wait")
        q = c["queries"]
        require(q["oracle_rays"] == 1024 and q["oracle"] == ("analytic_boxes" if c["name"] in ("islands", "root_tiles") else "dense_dda"), "missing independent oracle")
        require(len(q["samples"]) == samples + 2 and [x["phase"] for x in q["samples"]] == ["warmup"] * 2 + ["steady"] * samples, "missing query warmup/steady sample")
        for pair in q["samples"]:
            for variant in ("tuned", "baseline"):
                profile(pair[variant]["gpu"])
                require(pair[variant]["rays"] == 1024, "unmatched query workload")
            require(pair["tuned"]["visited"] == pair["baseline"]["visited"], "transition tuning changed traversal")
        ablation = q["ablation"]
        for variant in ("hdda", "cell_step"):
            require(ablation[variant]["rays"] == 32, "unmatched ablation workload")
            profile(ablation[variant]["gpu"])
        require(ablation["cell_step"]["visited"] >= ablation["hdda"]["visited"], "invalid HDDA work reduction")
        require(len(c["renders"]) == len(expected_views) and {(r["width"], r["height"], r["projection"]) for r in c["renders"]} == expected_views, "missing/duplicate view")
        for r in c["renders"]:
            require(len(r["frames"]) == samples + 3 and [f["phase"] for f in r["frames"]] == ["cold"] + ["warmup"] * 2 + ["steady"] * samples, "missing cold/warmup/steady frame")
            for f in r["frames"]:
                profile(f["gpu"])
                for key in ("cpu_submit_ms", "wait_ms", "readback_ms", "end_to_end_ms"):
                    finite(f[key], key)
                require(abs(f["end_to_end_ms"] - sum(f[k] for k in ("cpu_submit_ms", "wait_ms", "readback_ms"))) < 1e-4, "wall-time intervals do not partition frame")
            require(r["old_and_pending_frame"]["allocation_bytes"] > r["after_frames"]["allocation_bytes"], "old/pending frame not observed")
            if c["name"] not in ("islands", "root_tiles"):
                quality = r["quality"]
                require(quality["pixels"] == 1024 and quality["depth_pixels"] == 1024 and quality["depth_extent"] == [256, 256*r["height"]//r["width"]]
                        and quality["maximum_color_error_bytes"] <= 1 and quality["maximum_depth_error"] <= 2e-6, "raster quality failed")
    return d


def describe(values):
    values = sorted(values)
    return {"count": len(values), "median": statistics.median(values), "p95": values[math.ceil(.95 * len(values)) - 1], "min": values[0], "max": values[-1]}


def aggregate(runs):
    require(len(runs) == 3 and {r["round"] for r in runs} == {0, 1, 2}, "three distinct rounds required")
    for r in runs:
        validate_run(r)
        require(r.get("measurement_version", 1) == runs[0].get("measurement_version", 1), "unmatched measurement protocol")
        for key in ("device", "configuration", "compiler", "shader_fingerprint", "matrix_version", "query_reflection"):
            require(r[key] == runs[0][key], f"unmatched {key}")
    output = {"schema": 1, "status": "passed", "rounds": 3, "measurement_version": runs[0].get("measurement_version", 1),
              "device": runs[0]["device"], "shader_fingerprint": runs[0]["shader_fingerprint"], "cases": []}
    if output["measurement_version"] == 2:
        output["precondition_wall_ms"] = [sum(f["end_to_end_ms"] for f in r["precondition"]["frames"]) for r in runs]
    for name in sorted(CASES):
        cases = [next(c for c in r["cases"] if c["name"] == name) for r in runs]
        for c in cases[1:]:
            for key in ("storage", "domain_min", "domain_max", "dense_reference_bytes"):
                require(c[key] == cases[0][key], f"{name}: unstable {key}")
            require(c["queries"]["checksum"] == cases[0]["queries"]["checksum"], "query output changed across rounds")
        tuned = [s["tuned"]["gpu"]["submission_ms"] for c in cases for s in c["queries"]["samples"] if s["phase"] == "steady"]
        base = [s["baseline"]["gpu"]["submission_ms"] for c in cases for s in c["queries"]["samples"] if s["phase"] == "steady"]
        row = {"name": name, "storage": cases[0]["storage"], "conversion_ms": describe([c["conversion_ms"] for c in cases]),
               "source_build_ms": describe([c["source_build_ms"] for c in cases]), "upload_gpu_ms": describe([c["upload"]["gpu"]["submission_ms"] for c in cases]),
               "conversion_sampled_working_peak": max(c["conversion_process_memory"]["sampled_working_peak"] for c in cases),
               "conversion_sampled_private_peak": max(c["conversion_process_memory"]["sampled_private_peak"] for c in cases),
               "old_generation_allocation_bytes": max(c["old_generation_and_upload"]["allocation_bytes"] for c in cases),
               "tuned_query_gpu_ms": describe(tuned), "baseline_query_gpu_ms": describe(base),
               "transition_speed_ratio": statistics.median(base) / statistics.median(tuned), "ablation": cases[0]["queries"]["ablation"], "renders": []}
        for first in cases[0]["renders"]:
            views = [next(v for v in c["renders"] if (v["width"], v["height"], v["projection"]) == (first["width"], first["height"], first["projection"])) for c in cases]
            require(all(v["checksum"] == first["checksum"] for v in views), "frame output changed across rounds")
            frames = [f for v in views for f in v["frames"] if f["phase"] == "steady"]
            row["renders"].append({"width": first["width"], "height": first["height"], "projection": first["projection"], "checksum": first["checksum"],
                "gpu_ms": describe([f["gpu"]["submission_ms"] for f in frames]), "cpu_submit_ms": describe([f["cpu_submit_ms"] for f in frames]),
                "end_to_end_ms": describe([f["end_to_end_ms"] for f in frames]), "cold_end_to_end_ms": describe([v["frames"][0]["end_to_end_ms"] for v in views]),
                "old_pending_allocation_bytes": max(v["old_and_pending_frame"]["allocation_bytes"] for v in views)})
        output["cases"].append(row)
    return output


def markdown(summary):
    lines = ["# M23.7 三轮实测汇总", "", f"设备：{summary['device']['name']}；驱动：{summary['device']['driver']}。", "",
        f"测量协议 v{summary['measurement_version']}：Release、validation OFF；每视图首帧 + 2 预热 + 8 采样帧，表中为三轮 24 样本。GPU 为 submission 时间，端到端包括 CPU 提交、等待和读回。v2 另有固定 64 帧设备预热，其全部耗时保留在原始报告。", "",
        "| 输入 | Nano MiB | 转换 ms 中位数 | 上传 GPU ms | 调优/旧转换 GPU 比率（旧/新） | HDDA/逐格访问区域 |", "| --- | --- | --- | --- | --- | --- |"]
    for c in summary["cases"]:
        a = c["ablation"]
        lines.append(f"| {c['name']} | {c['storage']['nano_bytes']/2**20:.4f} | {c['conversion_ms']['median']:.3f} | {c['upload_gpu_ms']['median']:.4f} | {c['transition_speed_ratio']:.3f} | {a['hdda']['visited']} / {a['cell_step']['visited']} |")
    lines += ["", "大域逐格对照限制在 t≤4096、32 条相同射线；该工作量收益不代表完整画面加速。短查询有计时噪声，收益和退化均保留。", "",
        "| 输入 | 视口 | 投影 | GPU ms 中位/p95 | CPU 提交 ms 中位 | 端到端 ms 中位/p95 | 旧帧+新帧 allocation MiB |", "| --- | --- | --- | --- | --- | --- | --- |"]
    for c in summary["cases"]:
        for r in c["renders"]:
            lines.append(f"| {c['name']} | {r['width']}×{r['height']} | {r['projection']} | {r['gpu_ms']['median']:.3f}/{r['gpu_ms']['p95']:.3f} | {r['cpu_submit_ms']['median']:.3f} | {r['end_to_end_ms']['median']:.3f}/{r['end_to_end_ms']['p95']:.3f} | {r['old_pending_allocation_bytes']/2**20:.2f} |")
    lines += ["", "| 输入 | source/normalized tree MiB | 已知转换同时存活 payload MiB | 转换期采样 working/private MiB | 旧代+新代上传 allocation MiB |", "| --- | --- | --- | --- | --- |"]
    for c in summary["cases"]:
        s = c["storage"]
        lines.append(f"| {c['name']} | {s['source_tree_bytes']/2**20:.3f}/{s['normalized_tree_bytes']/2**20:.3f} | {s['conversion_live_payload_bytes']/2**20:.3f} | {c['conversion_sampled_working_peak']/2**20:.1f}/{c['conversion_sampled_private_peak']/2**20:.1f} | {c['old_generation_allocation_bytes']/2**20:.2f} |")
    lines += ["", "进程采样间隔 1 ms，只给出观测下界，包含运行时/编译器/驱动及分配器保留量；不等同 OpenVDB 内部 scratch 的精确峰值。VMA allocation 与 block 分列在原始 JSON；device-local 和 host-visible 可重叠，不能相加。AS/AABB/SBT 不适用。所有用例和进程释放后的 allocation 数均恢复基线。", ""]
    return "\n".join(lines)


def plot(summary, output):
    # Optional reporting dependency; collection/validation use only Python stdlib.
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    cases = summary["cases"]
    names = [c["name"] for c in cases]
    x = list(range(len(names)))
    fig, axes = plt.subplots(1, 2, figsize=(15, 5.5), constrained_layout=True)
    for width, color in [(640, "#2878b5"), (1280, "#d97932")]:
        values = [next(r["gpu_ms"]["median"] for r in c["renders"] if r["width"] == width and r["projection"] == "orthographic") for c in cases]
        axes[0].plot(x, values, "o-", label=f"{width}px wide", color=color)
    axes[0].set(title="Raster GPU submission: orthographic", ylabel="Median milliseconds (24 samples)")
    axes[0].set_yscale("log")
    for field, label, color in [("nano_bytes", "NanoVDB CPU/GPU payload each", "#2878b5"),
                                ("conversion_live_payload_bytes", "Known live conversion payload", "#d97932")]:
        axes[1].plot(x, [c["storage"][field]/2**20 for c in cases], "o-", label=label, color=color)
    axes[1].set(title="Sparse storage and conversion payload", ylabel="MiB (log scale)")
    axes[1].set_yscale("log")
    for ax in axes:
        ax.set_xticks(x, names, rotation=55, ha="right")
        ax.grid(axis="y", alpha=.25)
        ax.legend(fontsize=8)
    fig.suptitle(f"M23.7 / {summary['device']['name']} / Release / validation off", fontsize=12)
    fig.savefig(output / "baseline.svg")
    fig.savefig(output / "baseline.png", dpi=150)
    plt.close(fig)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--exe", type=Path)
    p.add_argument("--output", type=Path, required=True)
    p.add_argument("--aggregate-only", type=Path, nargs=3)
    p.add_argument("--plot", action="store_true", help="also write SVG/PNG charts (requires matplotlib)")
    a = p.parse_args()
    a.output.mkdir(parents=True, exist_ok=True)
    paths = a.aggregate_only
    if not paths:
        require(a.exe and a.exe.is_file(), "--exe must be a built Release benchmark")
        paths = []
        env = dict(os.environ, DISABLE_LAYER_AMD_SWITCHABLE_GRAPHICS_1="1")
        for i in range(3):
            path = a.output / f"round-{i}.json"
            with (a.output / f"round-{i}.log").open("w", encoding="utf-8") as log:
                subprocess.run([str(a.exe.resolve()), "--round", str(i), "--output", str(path.resolve())], env=env, stdout=log, stderr=subprocess.STDOUT, check=True, timeout=1800)
            validate_run(json.loads(path.read_text(encoding="utf-8")))
            paths.append(path)
    runs = [read_run(path) for path in paths]
    summary = aggregate(runs)
    summary["raw_sources"] = [{"name": p.name, "sha256": hashlib.sha256(p.read_bytes()).hexdigest()} for p in paths]
    if a.exe:
        summary["executable_sha256"] = hashlib.sha256(a.exe.read_bytes()).hexdigest()
    (a.output / "summary.json").write_text(json.dumps(summary, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    (a.output / "summary.md").write_text(markdown(summary), encoding="utf-8")
    if a.plot:
        plot(summary, a.output)
    print(f"Passed: 3 rounds, {len(CASES)} inputs, 48 views per round; {a.output / 'summary.json'}")


if __name__ == "__main__":
    main()
