"""Aggregate saved Sponza probe data; render linear RGB with the editor tone curve."""
import argparse
from collections import defaultdict
import json
from pathlib import Path
import statistics
import numpy as np
from PIL import Image, ImageDraw, ImageFont


def load(path):
    return json.loads(path.read_text(encoding="utf-8"))


def pixels(path, info):
    data = np.fromfile(path, dtype="<f4").reshape(info["height"], info["width"], 3).astype(float)
    assert np.isfinite(data).all() and (data >= 0).all(), path
    return data


def chroma(data):
    return data / np.maximum(data.sum(axis=2, keepdims=True), 1e-20)


def display(data, size=None):
    linear = data * 2**2.5
    tone = 1 - 1 / (1 + linear)
    # Normal sRGB swapchain: GPU stores tone-mapped linear RGBA8, GUI filters
    # that texture, then the attachment encodes sRGB. Keep the same order.
    quantized = Image.fromarray(np.rint(tone * 255).clip(0, 255).astype("uint8"))
    if size:
        quantized = quantized.resize(size, Image.Resampling.BILINEAR)
    tone = np.asarray(quantized).astype(float) / 255
    srgb = np.where(tone <= .0031308, tone * 12.92, 1.055 * tone**(1/2.4) - .055)
    return Image.fromarray(np.rint(srgb * 255).clip(0, 255).astype("uint8"))


def montage(path, rows, titles, footnote):
    width, height = 480, 290
    canvas = Image.new("RGB", (len(titles) * width, len(rows) * (height + 55) + 70), "#171c22")
    draw = ImageDraw.Draw(canvas)
    try:
        font = ImageFont.truetype("C:/Windows/Fonts/arial.ttf", 20)
        small = ImageFont.truetype("C:/Windows/Fonts/arial.ttf", 16)
    except OSError:
        font = small = ImageFont.load_default()
    for j, title in enumerate(titles):
        draw.text((12 + j * width, 8), title, font=font, fill="white")
    for i, (label, images) in enumerate(rows):
        y = 42 + i * (height + 55)
        draw.text((12, y), label, font=font, fill="#cbd8e9")
        for j, data in enumerate(images):
            canvas.paste(display(data, (width, height)), (j * width, y + 30))
    draw.text((12, canvas.height - 24), footnote, font=small, fill="#cbd8e9")
    canvas.save(path)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("inputs", nargs="+", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    groups = defaultdict(list)
    quality = []
    quality_rows = []
    motions = defaultdict(dict)
    sources = []
    for directory in args.inputs:
        manifest = load(directory / "manifest.json")
        sources.append(dict(directory=str(directory.resolve()), editor_sha256=manifest["editor_sha256"],
                            worker_sha256=manifest["worker_sha256"], head=manifest["head"]))
        for run in manifest["runs"]:
            report_path = directory / run["path"].replace("\\", "/")
            report = load(report_path)
            assert report["status"] == "passed", report_path
            config = run["config"]
            if run["color"]:
                for pose in (0, 1):
                    for seed in (1, 17):
                        images = {}
                        for case in ("old1", "old3", "new3", "reference96"):
                            stem = f"{case}-p{pose}-s{seed}"
                            info = load(report_path.parent / (stem + ".json"))
                            images[case] = pixels(report_path.parent / (stem + ".rgb32f"), info)
                        ref = images["reference96"]
                        ref_c = chroma(ref)
                        mask = (ref.sum(axis=2) > 1e-5) & (ref_c.max(axis=2) < .8)
                        assert mask.sum() > ref.shape[0] * ref.shape[1] / 2, "Reference mask too small"
                        # Chroma is undefined at zero radiance. Compare the same
                        # positive support, and report its coverage separately.
                        common = mask.copy()
                        for case in ("old1", "old3", "new3"):
                            common &= images[case].sum(axis=2) > 1e-5
                        assert common.any(), "No common positive color support"
                        for case in ("old1", "old3", "new3"):
                            c = chroma(images[case])
                            quality.append(dict(algorithm=config["algorithm"], pose=pose, seed=seed, case=case,
                                                mask_pixels=int(mask.sum()),
                                                common_pixels=int(common.sum()), common_percent=float(common.mean()*100),
                                                positive_percent=float(np.mean(images[case].sum(axis=2)>1e-5)*100),
                                                rgb_relative_rmse=float(np.sqrt(np.sum((images[case]-ref)**2)/np.sum(ref**2))),
                                                chroma_rmse=float(np.sqrt(np.mean(np.sum((c[common]-ref_c[common])**2, axis=1)))),
                                                dominant_percent=float(np.mean(c[common].max(axis=1) > .95)*100)))
                        if pose == 0 and seed == 1:
                            quality_rows.append((config["algorithm"].upper() + " | yaw 85 deg | seed 1", list(images.values())))
            elif config["images"]:
                if not run["validation"]:
                    info = load(report_path.parent / "motion-0.json")
                    motions[config["algorithm"]][config["legacy"]] = pixels(report_path.parent / "motion-0.rgb32f", info)
            elif not run["validation"]:
                frames = report["frames"]
                stats = dict(name=run["name"], frames=len(frames), inputs=len(report["inputs"]),
                             updates_per_second=len(frames)/report["measured_seconds"], full_size=report["full_size"],
                             motion_size=[frames[0]["width"], frames[0]["height"]],
                             stop_to_full_ms=report["stop_to_full_ms"], stop_to_next_batch_ms=report["stop_to_next_batch_ms"],
                             cold_ready_ms=report["cold_ready_ms"], spp_range=[min(f["spp"] for f in frames),max(f["spp"] for f in frames)])
                for key, values in (("input", [f["input_ms"] for f in frames]),
                                    ("request", [f["request_ms"] for f in frames]),
                                    ("gui", report["gui_intervals_ms"])):
                    for percentile in (50, 95):
                        stats[f"{key}_p{percentile}_ms"] = float(np.percentile(values, percentile))
                groups[(config["preset"], config["algorithm"], "old" if config["legacy"] else "new")].append(stats)
    summary = dict(sources=sources, performance=[], quality=quality, quality_summary=[])
    for (preset, algorithm, policy), runs in groups.items():
        numeric = [k for k, v in runs[0].items() if isinstance(v, (int, float))]
        summary["performance"].append(dict(preset=preset, algorithm=algorithm, policy=policy, runs=runs,
                                          median={k: statistics.median(r[k] for r in runs) for k in numeric}))
    for algorithm in ("classic", "restir"):
        for case in ("old1", "old3", "new3"):
            entries = [r for r in quality if r["algorithm"] == algorithm and r["case"] == case]
            if entries:
                summary["quality_summary"].append(dict(algorithm=algorithm, case=case,
                    mean_chroma_rmse=statistics.mean(r["chroma_rmse"] for r in entries),
                    mean_dominant_percent=statistics.mean(r["dominant_percent"] for r in entries),
                    mean_common_percent=statistics.mean(r["common_percent"] for r in entries),
                    mean_positive_percent=statistics.mean(r["positive_percent"] for r in entries),
                    mean_rgb_relative_rmse=statistics.mean(r["rgb_relative_rmse"] for r in entries)))
    (args.output / "summary.json").write_text(json.dumps(summary, indent=2), encoding="utf-8")
    if quality_rows:
        montage(args.output / "color-comparison.png", quality_rows,
                ["Old: 1 spp", "Old: 3 spp (equal work)", "New: 3 spp", "96 spp reference"],
                "Sponza 192 x 116 | EV +2.5, Reinhard + sRGB | Reference is finite, not certified converged")
    if motions:
        montage(args.output / "motion-comparison.png", [(a.upper(), [r[True], r[False]]) for a, r in motions.items()],
                ["Old: live motion", "New: live motion"],
                "Actual moving viewport frames; capture passes use readback and are excluded from timing")
    for row in summary["performance"]:
        print(row["preset"], row["algorithm"], row["policy"], json.dumps(row["median"]))
    print(json.dumps(summary["quality_summary"], indent=2))


if __name__ == "__main__":
    main()
