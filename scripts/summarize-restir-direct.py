"""Summarize native motion logs and RGB direct matched-camera finite references."""
import argparse
import json
from pathlib import Path
import numpy as np
from PIL import Image, ImageDraw, ImageFont


def load(path):
    return json.loads(path.read_text(encoding="utf-8"))


def percentiles(values):
    return dict(zip(("p50", "p95", "max"), map(float, np.percentile(values, [50, 95, 100]))))


def pixels(path):
    source = load(path)
    image = np.fromfile(path.with_suffix(".rgb32f"), dtype="<f4").reshape(source["height"], source["width"], 3).astype(float)
    assert np.isfinite(image).all() and (image >= 0).all()
    return source, image


def display(image):
    image = image*2**2.5
    image = image/(1+image)
    return np.clip(np.where(image <= .0031308, 12.92*image, 1.055*image**(1/2.4)-.055), 0, 1)


def error(image, reference):
    delta = image-reference
    a, b = display(image), display(reference)
    # Chroma error after removing each pixel's mean channel error: color versus brightness.
    d = a-b
    return dict(hdr_rmse=float(np.sqrt(np.mean(delta**2))),
                hdr_nrmse=float(np.sqrt(np.mean(delta**2)/np.mean(reference**2))),
                display_rmse=float(np.sqrt(np.mean(d**2))),
                display_chroma_rmse=float(np.sqrt(np.mean((d-d.mean(axis=2, keepdims=True))**2))),
                mean_rgb=image.mean(axis=(0, 1)).tolist(), reference_mean_rgb=reference.mean(axis=(0, 1)).tolist(),
                mean_relative_error=((image.mean(axis=(0, 1))-reference.mean(axis=(0, 1)))/reference.mean(axis=(0, 1))).tolist())


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("root", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    manifest = load(args.root/"manifest.json")
    provenance = {key: manifest[key] for key in
                  ("head", "gpu", "editor_sha256", "worker_sha256", "reference_worker_sha256", "reference_probe_sha256")
                  if key in manifest}
    summary = dict(provenance=provenance, performance=[], quality=[], limitations=[
        "Input latency ends at completed GUI GPU submission; display scanout is excluded.",
        "Quality is descriptive at three captured moving cameras per policy, not an independent-seed confidence interval.",
        "256/512 spp Classic MIS references are finite; their difference is reported, not certified ground truth.",
        "All comparisons are full RGB surface direct only; no volumetric or indirect lighting and no denoising."])
    for row in manifest["runs"]:
        report = load(args.root/row["path"])
        if row["images"] or row["validation"]:
            continue
        frames = report["frames"]
        summary["performance"].append(dict(name=row["name"], policy=row["policy"], size=report["full_size"],
            updates_per_second=len(frames)/report["measured_seconds"],
            input_ms=percentiles([f["input_ms"] for f in frames]), request_ms=percentiles([f["request_ms"] for f in frames]),
            gui_ms=percentiles(report["gui_intervals_ms"]), stop_to_current_ms=report["stop_to_full_ms"],
            stop_to_next_batch_ms=report["stop_to_next_batch_ms"],
            temporal_reuse_fraction=float(np.mean([f["temporal_reused"]/max(1, f["width"]*f["height"]*f["spp"]) for f in frames]))))
    groups = {}
    for row in summary["performance"]:
        key = row["policy"]+"-"+"x".join(map(str, row["size"]))
        groups.setdefault(key, []).append(row)
    summary["performance_medians"] = {k: dict(rounds=len(v), updates_per_second=float(np.median([r["updates_per_second"] for r in v])),
        input_p50_ms=float(np.median([r["input_ms"]["p50"] for r in v])), input_p95_ms=float(np.median([r["input_ms"]["p95"] for r in v])),
        stop_to_current_ms=float(np.median([r["stop_to_current_ms"] for r in v])),
        temporal_reuse_fraction=float(np.median([r["temporal_reuse_fraction"] for r in v]))) for k, v in groups.items()}
    panels = []
    if manifest.get("references"):
        refdir = (args.root/manifest["references"]).parent
        for row in manifest["runs"]:
            if not row["images"] or row["validation"]:
                continue
            for path in sorted((args.root/row["path"]).parent.glob("motion-*.json")):
                source, image = pixels(path)
                stem = row["policy"]+"-"+path.stem
                ref_source, reference = pixels(refdir/(stem+"-ref512.json"))
                lower_source, lower = pixels(refdir/(stem+"-ref256.json"))
                assert source["spp"] == 1 and source["bounces"] == 1
                assert lower_source["seed"] == ref_source["seed"] != source["seed"]
                for ref, spp in ((lower_source, 256), (ref_source, 512)):
                    assert ref["spp"] == spp and not ref["restir"]
                    assert all(source[key] == ref[key] for key in
                               ("geometry", "fingerprint", "width", "height", "lens", "bounces", "sampler", "exposure_ev"))
                    assert np.allclose(source["camera_world"], ref["camera_world"], rtol=0, atol=1e-10)
                summary["quality"].append(dict(policy=row["policy"], frame=path.stem, source=source,
                    errors=error(image, reference), reference_256_to_512=error(lower, reference)))
                if path.stem == "motion-1":
                    panels.append((row["policy"], image, reference))
        summary["quality_means"] = {policy: {metric: float(np.mean([r["errors"][metric] for r in summary["quality"] if r["policy"] == policy]))
            for metric in ("hdr_rmse", "hdr_nrmse", "display_rmse", "display_chroma_rmse")} for policy in ("classic", "v1", "v2")}
    if panels:
        canvas = Image.new("RGB", (640*len(panels), 640*2+70), "#161b22")
        draw = ImageDraw.Draw(canvas)
        font = ImageFont.load_default(size=22)
        labels = {"classic": "Classic MIS", "v1": "ReSTIR v1", "v2": "ReSTIR v2"}
        for col, (label, image, reference) in enumerate(panels):
            draw.text((col*640+12, 4), f"{labels[label]}  /  moving, 1 spp", font=font, fill="white")
            for line, frame in enumerate((image, reference)):
                bitmap = Image.fromarray(np.uint8(display(frame)*255)).resize((640, 640), Image.Resampling.BILINEAR)
                canvas.paste(bitmap, (col*640, 35+line*675))
            draw.text((col*640+12, 679), "Matched camera  /  Classic MIS, 512 spp", font=font, fill="white")
        canvas.save(args.output/"moving-comparison.png")
    (args.output/"summary.json").write_text(json.dumps(summary, indent=2, ensure_ascii=False), encoding="utf-8")
    print(json.dumps({k: v for k, v in summary.items() if k in ("performance_medians", "quality_means")}, indent=2))


if __name__ == "__main__":
    main()
