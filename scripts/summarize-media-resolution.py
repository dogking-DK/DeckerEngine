"""Summarize fresh-camera throughput and compare matched moving images on the native grid."""
import argparse
import json
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFont


def read(path):
    return json.loads(path.read_text(encoding="utf-8"))


def shown(path, size):
    source = read(path)
    rgb = np.fromfile(path.with_suffix(".rgb32f"), dtype="<f4").reshape(source["height"], source["width"], 3)
    assert np.isfinite(rgb).all() and rgb.min() >= 0
    # The sRGB swapchain receives the linear, 8-bit tone pass through the GUI sampler.
    mapped = np.rint(255 * (1 - 1 / (1 + rgb * 2**source["exposure_ev"]))) / 255
    if size != (source["width"], source["height"]):
        mapped = np.stack([np.asarray(Image.fromarray(c).resize(size, Image.Resampling.BILINEAR))
                           for c in mapped.transpose(2, 0, 1)], axis=-1)
    return source, np.where(mapped <= .0031308, 12.92*mapped, 1.055*mapped**(1/2.4)-.055)


def quality(image, ref):
    # Equal-size 8x8 blocks, Rec.709 luma on encoded display RGB, unit data range.
    def blocks(rgb):
        y = rgb @ np.array([.2126, .7152, .0722], dtype=np.float32)
        h, w = (v // 8 for v in y.shape)
        return y[:h*8, :w*8].reshape(h, 8, w, 8).transpose(0, 2, 1, 3).reshape(h, w, 64)
    x, y = blocks(image), blocks(ref)
    a, b = x.mean(axis=-1), y.mean(axis=-1)
    x, y = x-a[..., None], y-b[..., None]
    ssim = ((2*a*b+.01**2) * (2*(x*y).mean(axis=-1)+.03**2) /
            ((a*a+b*b+.01**2) * ((x*x+y*y).mean(axis=-1)+.03**2)))
    delta = image-ref
    h, w = (v // 8 for v in delta.shape[:2])
    block_delta = delta[:h*8, :w*8].reshape(h, 8, w, 8, 3).mean(axis=(1, 3))
    return dict(display_rmse=float(np.sqrt(np.mean(delta**2))), block_luma_ssim=float(ssim.mean()),
                block_rgb_rmse=float(np.sqrt(np.mean(block_delta**2))))


def plate(images, output, size):
    w, h = size
    pw, ph = 364, round(364*h/w)
    cw, ch = 340, 215
    left, top = round(w*.58)-cw//2, round(h*.41)-ch//2
    box = (left, top, left+cw, top+ch)
    sheet = Image.new("RGB", ((pw+8)*4+8, ph+ch+90), "#17212b")
    draw = ImageDraw.Draw(sheet)
    font = ImageFont.load_default(size=19)
    for i, (label, rgb) in enumerate(images):
        frame = Image.fromarray(np.clip(np.rint(rgb*255), 0, 255).astype("uint8"))
        overview = frame.resize((pw, ph), Image.Resampling.LANCZOS)
        ImageDraw.Draw(overview).rectangle(tuple(round(v*pw/w) for v in box), outline="#f3b859", width=2)
        x = 8+(pw+8)*i
        draw.text((x, 5), label, font=font, fill="white")
        sheet.paste(overview, (x, 34))
        sheet.paste(frame.crop(box), (x, ph+65))
    draw.text((8, ph+37), "Native-grid detail, 1:1  |  same camera and crop", font=font, fill="#e6b86f")
    sheet.save(output)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("root", type=Path)
    parser.add_argument("prefix", type=Path)
    args = parser.parse_args()
    manifest = read(args.root / "manifest.json")
    summary = dict(head=manifest["head"], gpu=manifest["gpu"], seconds=manifest["identity"]["seconds"],
                   editor_sha256=manifest["identity"]["editor"], worker_sha256=manifest["reference"]["worker_sha256"],
                   performance={}, quality={}, reference_48_to_96=[], aborted_attempts=[])
    native = tuple(read(args.root / "quality-0/project/.dk-media-motion/report.json")["full_size"])
    refdir = args.root / manifest["reference"]["directory"] / "images"
    assert read(refdir / "report.json")["status"] == "passed"
    plate_images = []
    for cap in (0, 640, 192):
        runs = []
        for r in range(3):
            base = args.root / f"perf-{r}-{cap}"
            attempts = [base] + sorted(args.root.glob(base.name+"-attempt*"))
            for directory in attempts:
                error = (directory / "stderr.log").read_text(errors="replace").strip()
                if error:
                    summary["aborted_attempts"].append(dict(run=directory.name, reason=error))
            report = read(attempts[-1] / "project/.dk-media-motion/report.json")
            assert tuple(report["full_size"]) == native and report["status"] == "passed"
            frames = report["frames"]
            delta = np.diff([f["at_ms"] for f in frames])
            assert delta.min() > 0 and len({f["camera"] for f in frames}) == len(frames)
            latency = [f["input_ms"] for f in frames]
            runs.append(dict(frames=len(frames), seconds=report["measured_seconds"],
                             fps=(len(frames)-1)*1000/sum(delta), per_window=len(frames)/report["measured_seconds"],
                             input_p50_ms=float(np.median(latency)), input_p95_ms=float(np.percentile(latency, 95)),
                             gui_fps=1000/np.mean(report["gui_intervals_ms"]), stop_to_full_ms=report["stop_to_full_ms"],
                             size=[frames[0]["width"],frames[0]["height"]], spp=sorted({f["spp"] for f in frames})))
        summary["performance"][cap] = dict(runs=runs, median={k:float(np.median([r[k] for r in runs]))
                                              for k in ("fps","per_window","input_p50_ms","input_p95_ms","stop_to_full_ms","gui_fps")})
        metrics = []
        for i in range(3):
            path = args.root / f"quality-{cap}/project/.dk-media-motion/motion-{i}.json"
            source, image = shown(path, native)
            ref, reference = shown(refdir / f"motion-{i}-ref96.json", native)
            assert (ref["width"], ref["height"]) == native and ref["spp"] == 96 and ref["restir_version"] == 0
            assert source["seed"] != ref["seed"] and source["spp"] == 3 and source["restir_version"] == 2
            assert source["sampler"] == ref["sampler"] == 3 and source["bounces"] == ref["bounces"] == 4
            assert source["field_hash"] == ref["field_hash"] != 0 and source["geometry"] == ref["geometry"]
            assert source["lens"] == ref["lens"] and np.allclose(source["camera_world"], ref["camera_world"], rtol=0, atol=1e-10)
            metrics.append(dict(yaw=source["pose"]["yaw"], **quality(image, reference)))
            if cap == 0:
                lower, pixels = shown(refdir / f"motion-{i}-ref48.json", native)
                assert lower["spp"] == 48 and lower["seed"] == ref["seed"] and lower["camera_world"] == ref["camera_world"]
                summary["reference_48_to_96"].append(quality(pixels, reference))
            if i == 1:
                plate_images.append((f'{source["width"]} x {source["height"]}  |  motion, 3 spp', image))
                Image.fromarray(np.rint(image*255).clip(0, 255).astype("uint8")).save(args.root / f"display-{cap}.png")
                if cap == 192:
                    plate_images.append(("Native  |  Classic MIS, 96 spp", reference))
                    Image.fromarray(np.rint(reference*255).clip(0, 255).astype("uint8")).save(args.root / "display-reference.png")
        summary["quality"][cap] = dict(poses=metrics, mean={k:float(np.mean([m[k] for m in metrics]))
                                       for k in ("display_rmse", "block_luma_ssim", "block_rgb_rmse")})
    plate(plate_images, args.prefix.with_suffix(".png"), native)
    args.prefix.with_suffix(".json").write_text(json.dumps(summary, indent=2)+"\n", encoding="utf-8")


if __name__ == "__main__":
    main()
