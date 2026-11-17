"""Sponza native-window motion and frozen-camera color evidence; runs serially."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import time
from uuid import uuid4


def write(path, value):
    path.write_text(json.dumps(value, indent=2, ensure_ascii=False), encoding="utf-8")


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run(args, name, algorithm, preset, legacy=False, images=False, validation=False, color=False):
    directory = args.output / name
    directory.mkdir()
    root = directory / "project"
    shutil.copytree(args.project, root, ignore=shutil.ignore_patterns("captures", ".dk-cache", ".cache"))
    (root / ".dk-editor-smoke").touch()
    config = dict(algorithm=algorithm, preset=preset, legacy=legacy, images=images,
                  seconds=2 if validation or images else args.seconds, height=116)
    write(root / ".dk-media-motion.json", config)
    evidence = directory / "color" if color else root / ".dk-media-motion"
    command = ([str(args.worker), str(root), str(evidence), "--media-color"] if color else
               [str(args.editor), "--background", "--root", str(root), "--media-motion-probe"])
    if validation:
        command.append("--validation")
    elif not color:
        command.append("--no-validation")
    write(directory / "command.json", command)
    env = os.environ.copy()
    env["DISABLE_LAYER_AMD_SWITCHABLE_GRAPHICS_1"] = "1"
    if validation:
        env.update(VK_LAYER_VALIDATE_SYNC="1", VK_VALIDATION_VALIDATE_SYNC="1")
    else:
        env.pop("VK_LAYER_VALIDATE_SYNC", None)
        env.pop("VK_VALIDATION_VALIDATE_SYNC", None)
    start = time.monotonic()
    with (directory / "stdout.log").open("wb") as out, (directory / "stderr.log").open("wb") as err:
        result = subprocess.run(command, stdin=subprocess.DEVNULL, stdout=out, stderr=err,
                                env=env, timeout=600 if color else 240,
                                creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
    assert result.returncode == 0, f"{name}: {result.returncode}: {(directory / 'stderr.log').read_text(errors='replace')}"
    report = json.loads((evidence / "report.json").read_text())
    assert report["status"] == "passed", report
    if not color:
        log = (directory / "stdout.log").read_text(errors="replace")
        assert "validation errors=0 warnings=0 liveAllocations=0" in log, log
        assert len(report["frames"]) >= 2 and len(report["inputs"]) >= 8, report
        assert all(f["sampler"] == (2 if legacy else 3) and f["spp"] >= (1 if legacy else 3)
                   and max(f["width"], f["height"]) == 192 and f["input_ms"] > 0 for f in report["frames"]), report
        assert report["next_spp"] > report["restored_source"]["spp"], report
        if images:
            assert len(list(evidence.glob("motion-*.rgb32f"))) >= 2
    print(f"{name}: passed ({time.monotonic()-start:.1f}s)", flush=True)
    row = dict(name=name, path=str((evidence / "report.json").relative_to(args.output)),
               config=config, validation=validation, color=color)
    return row


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for key in ("editor", "project", "output"):
        parser.add_argument("--" + key, type=Path, required=True)
    parser.add_argument("--worker", type=Path)
    parser.add_argument("--mode", choices=("validate", "performance", "images", "color", "all"), default="all")
    parser.add_argument("--seconds", type=float, default=6)
    parser.add_argument("--rounds", type=int, default=3)
    parser.add_argument("--presets", nargs="+", choices=("clear", "sparse"), default=["clear", "sparse"])
    args = parser.parse_args()
    assert 2 <= args.seconds <= 20 and 1 <= args.rounds <= 10
    args.output = args.output.resolve()
    if args.mode == "validate":
        args.output /= uuid4().hex
    args.output.mkdir(parents=True, exist_ok=False)
    repo = Path(__file__).resolve().parents[1]
    tracked = subprocess.check_output(["git", "ls-files", "engine", "apps/editor", "tests/integration", "scripts/benchmark-media-preview.py"], cwd=repo, text=True).splitlines()
    # Include newly added probe files before their first commit, too.
    tracked += [str(p.relative_to(repo)).replace("\\", "/") for p in (repo / "engine/editor/src").glob("Media*Probe*")]
    tracked += ["tests/integration/MediaColorProbe.inl", "scripts/benchmark-media-preview.py"]
    manifest = dict(editor_sha256=digest(args.editor), worker_sha256=digest(args.worker) if args.worker else None,
                    head=subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=repo, text=True).strip(),
                    sources={p: digest(repo / p) for p in sorted(set(tracked)) if (repo / p).is_file()},
                    project={str(p.relative_to(args.project)): digest(p) for folder in ("assets", "scenes")
                             for p in sorted((args.project / folder).rglob("*")) if p.is_file()}, runs=[])
    manifest["project"]["project.json"] = digest(args.project / "project.json")
    write(args.output / "manifest.json", manifest)
    def record(*pos, **kw):
        manifest["runs"].append(run(args, *pos, **kw))
        write(args.output / "manifest.json", manifest)
    if args.mode in ("validate", "all"):
        record("validation-restir", "restir", "clear", images=True, validation=True)
    if args.mode in ("performance", "all"):
        for preset in args.presets:
            for algorithm in ("classic", "restir"):
                for repeat in range(args.rounds):
                    for legacy in ((True, False) if repeat % 2 == 0 else (False, True)):
                        record(f"{preset}-{algorithm}-{'old' if legacy else 'new'}-{repeat}", algorithm, preset, legacy)
    if args.mode in ("images", "all"):
        for algorithm in ("classic", "restir"):
            for legacy in (True, False):
                record(f"images-{algorithm}-{'old' if legacy else 'new'}", algorithm, "clear", legacy, images=True)
    if args.mode in ("color", "all"):
        assert args.worker, "--worker required for color mode"
        for algorithm in ("classic", "restir"):
            record("color-" + algorithm, algorithm, "clear", color=True)


if __name__ == "__main__":
    main()
