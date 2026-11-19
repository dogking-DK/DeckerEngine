"""Measure Sponza Scene media/ReSTIR v2 at three motion preview resolutions."""
import argparse
import json
from pathlib import Path
import runpy
import subprocess


helpers = runpy.run_path(str(Path(__file__).with_name("benchmark-restir-direct.py")))
write, digest, execute, project = (helpers[key] for key in ("write", "digest", "execute", "project"))
caps = (0, 640, 192)
gpu_errors = ("render.pathtracing.medium_domain", "Invalid GPU path sample/history")


def run(args, name, cap, quality=False, validation=False):
    directory, root = project(args, name)
    config = dict(preset="sparse", algorithm="restir", legacy=False, images=quality, matched_poses=quality,
                  motion_preview_long_edge=cap, seconds=args.seconds)
    write(root / ".dk-media-motion.json", config)
    execute(directory, [str(args.editor), "--background", "--root", str(root), "--media-motion-probe",
                        "--validation" if validation else "--no-validation"], validation, timeout=420)
    evidence = root / ".dk-media-motion"
    report = json.loads((evidence / "report.json").read_text())
    assert report["status"] == "passed" and "validation errors=0 warnings=0 liveAllocations=0" in (directory / "stdout.log").read_text(errors="replace")
    settings = report["viewport"]
    assert settings["medium_mode"] == "scene_media" and settings["lighting_scope"] == "full_path"
    assert settings["motion_preview_long_edge"] == cap and settings["initial_candidates"] == 8
    assert settings["temporal_reuse"] and settings["spatial_reuse"] and settings["bounces"] == 4
    assert report["initial_source"]["field_hash"] and report["environment"]["enabled"] and report["environment"]["clouds"]["enabled"]
    width, height = report["full_size"]
    assert max(width, height) > max(caps)
    size = [v * cap // max(width, height) if cap else v for v in (width, height)]
    if quality:
        files = sorted(evidence.glob("motion-*.json"))
        assert len(files) == 3
        frames = [json.loads(p.read_text()) for p in files]
        assert [round(f["pose"]["yaw"]) for f in frames] == [89, 91, 93]
    else:
        frames = report["frames"]
        assert len(frames) >= 2 and len(report["inputs"]) >= 8 and report["warm_camera_updates"] >= 20
        assert report["restored_source"]["width"] == width and report["restored_source"]["height"] == height
        assert report["next_spp"] > report["restored_source"]["spp"]
    assert all(f["restir_version"] == 2 and f["sampler"] == 3 and [f["width"], f["height"]] == size for f in frames)
    print(f"{name}: {width}x{height} -> {size}, {len(frames)} frames", flush=True)


def timed_round(args, name, cap):
    # Motion samples a wall-clock input stream. Keep every numerical abort with its own logs.
    for attempt in range(1, 4):
        label = name if attempt == 1 else f"{name}-attempt{attempt}"
        directory = args.output / label
        if not directory.exists():
            try:
                run(args, label, cap)
                return
            except AssertionError:
                if not any(e in (directory / "stderr.log").read_text(errors="replace") for e in gpu_errors):
                    raise
        elif (directory / "project/.dk-media-motion/report.json").is_file():
            report = json.loads((directory / "project/.dk-media-motion/report.json").read_text())
            if report["status"] == "passed" and "validation errors=0 warnings=0 liveAllocations=0" in (directory / "stdout.log").read_text(errors="replace"):
                return
        error = (directory / "stderr.log").read_text(errors="replace").strip()
        assert any(e in error for e in gpu_errors), label
        print(f"{label}: {error} (logs retained)", flush=True)
    raise RuntimeError(f"{name}: three GPU aborts")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for key in ("editor", "worker", "project", "output"):
        parser.add_argument("--" + key, type=lambda p: Path(p).resolve(), required=True)
    parser.add_argument("--phase", choices=("validate", "performance", "quality", "references", "all"), default="all")
    parser.add_argument("--seconds", type=int, default=10, choices=range(2, 21))
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    project_files = [p for p in args.project.rglob("*") if p.is_file() and p.parts[len(args.project.parts)] in ("assets", "scenes", "project.json")]
    identity = dict(editor=digest(args.editor),
                    project={str(p.relative_to(args.project)): digest(p) for p in project_files}, seconds=args.seconds)
    manifest = args.output / "manifest.json"
    if manifest.exists():
        saved = json.loads(manifest.read_text())["identity"]
        assert all(saved[key] == value for key, value in identity.items()), "Editor, scene or interval differs"
    else:
        write(manifest, dict(identity=identity, head=subprocess.check_output(["git", "rev-parse", "HEAD"], text=True).strip(),
              gpu=subprocess.check_output(["nvidia-smi", "--query-gpu=name,driver_version,memory.total", "--format=csv,noheader"], text=True).strip()))
    if args.phase in ("all", "validate"):
        run(args, "validate", 192, quality=True, validation=True)
    if args.phase in ("all", "performance"):
        for r in range(3):
            for cap in caps[r:] + caps[:r]:
                timed_round(args, f"perf-{r}-{cap}", cap)
    if args.phase in ("all", "quality"):
        for cap in caps:
            run(args, f"quality-{cap}", cap, quality=True)
    if args.phase in ("all", "references"):
        name = "references"
        attempt = 1
        while (args.output / name).exists():
            attempt += 1
            name = f"references-attempt{attempt}"
        directory, root = project(args, name)
        sources = sorted((args.output / "quality-0/project/.dk-media-motion").glob("motion-*.json"))
        assert len(sources) == 3
        write(root / ".dk-direct-reference.json", dict(scene_media=True, sources=[dict(file=str(p), name=p.stem) for p in sources]))
        execute(directory, [str(args.worker), str(root), str(directory / "images"), "--direct-reference"], timeout=1800)
        data = json.loads(manifest.read_text())
        data["reference"] = dict(directory=name, worker_sha256=digest(args.worker))
        write(manifest, data)


if __name__ == "__main__":
    main()
