"""Serial native Sponza RGB direct motion measurements and matched-camera references."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import time


def write(path, value):
    path.write_text(json.dumps(value, indent=2, ensure_ascii=False), encoding="utf-8")


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def execute(directory, command, validation=False, timeout=300):
    env = os.environ.copy()
    env["DISABLE_LAYER_AMD_SWITCHABLE_GRAPHICS_1"] = "1"
    for name in ("VK_LAYER_VALIDATE_SYNC", "VK_VALIDATION_VALIDATE_SYNC"):
        env.pop(name, None)
        if validation:
            env[name] = "1"
    write(directory / "command.json", command)
    with (directory / "stdout.log").open("wb") as out, (directory / "stderr.log").open("wb") as err:
        result = subprocess.run(command, stdin=subprocess.DEVNULL, stdout=out, stderr=err, env=env,
                                timeout=timeout, creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
    assert result.returncode == 0, f"{directory.name}: {result.returncode}: {(directory / 'stderr.log').read_text(errors='replace')}"


def project(args, name):
    directory = args.output / name
    directory.mkdir()
    root = directory / "project"
    shutil.copytree(args.project, root, ignore=shutil.ignore_patterns("captures", ".dk-cache", ".cache"))
    (root / ".dk-editor-smoke").touch()
    return directory, root


def motion(args, name, policy, width, images=False, validation=False):
    directory, root = project(args, name)
    config = dict(surface_direct=True, algorithm="classic" if policy == "classic" else "restir", preset="clear",
                  legacy=policy == "v1", width=width, seconds=2 if images or validation else args.seconds, images=images)
    write(root / ".dk-media-motion.json", config)
    start = time.monotonic()
    execute(directory, [str(args.editor), "--background", "--root", str(root), "--media-motion-probe",
                        "--validation" if validation else "--no-validation"], validation)
    evidence = root / ".dk-media-motion"
    report = json.loads((evidence / "report.json").read_text())
    assert report["status"] == "passed", report
    assert "validation errors=0 warnings=0 liveAllocations=0" in (directory / "stdout.log").read_text(errors="replace")
    expected = {"classic": 0, "v1": 1, "v2": 2}[policy]
    assert len(report["frames"]) >= 2 and len(report["inputs"]) >= 8 and report["warm_camera_updates"] >= 20
    assert report["viewport"]["lighting_scope"] == "direct_only" and report["viewport"]["bounces"] == 1
    assert all(f["sampler"] == 1 and f["restir_version"] == expected and f["spp"] >= 1 and
               [f["width"], f["height"]] == report["full_size"] and f["input_ms"] > 0 for f in report["frames"])
    if width:
        assert max(report["full_size"]) == width
    assert report["next_spp"] > report["restored_source"]["spp"]
    if images:
        image_count = len(list(evidence.glob("motion-*.rgb32f")))
        assert image_count >= 2 if validation else image_count == 3
    print(f"{name}: passed ({time.monotonic()-start:.1f}s)", flush=True)
    return dict(name=name, policy=policy, width=width, images=images, validation=validation,
                path=str((evidence / "report.json").relative_to(args.output)))


def references(args, rows):
    name = "references"
    attempt = 1
    while (args.output / name).exists():
        name = f"references-rerun-{attempt}"
        attempt += 1
    directory, root = project(args, name)
    sources = []
    for row in rows:
        if row["images"] and not row["validation"]:
            for source in sorted((args.output / row["path"]).parent.glob("motion-*.json")):
                sources.append(dict(file=str(source), name=row["policy"]+"-"+source.stem))
    assert sources
    write(root / ".dk-direct-reference.json", dict(sources=sources))
    evidence = directory / "images"
    execute(directory, [str(args.worker), str(root), str(evidence), "--direct-reference"], timeout=1800)
    report = json.loads((evidence / "report.json").read_text())
    assert report["status"] == "passed" and not report["diagnostics"] and not report["live_allocations"]
    assert len(report["images"]) == 2*len(sources)
    print("Matched-camera 256/512 spp references: passed", flush=True)
    return str((evidence / "report.json").relative_to(args.output))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for key in ("editor", "worker", "project", "output"):
        parser.add_argument("--"+key, type=Path, required=True)
    parser.add_argument("--mode", choices=("validate", "performance", "quality", "references", "all"), default="all")
    parser.add_argument("--seconds", type=float, default=6)
    parser.add_argument("--rounds", type=int, default=3)
    parser.add_argument("--widths", nargs="+", type=int, default=[640, 0])
    args = parser.parse_args()
    assert 2 <= args.seconds <= 20 and 1 <= args.rounds <= 10
    args.output = args.output.resolve()
    repo = Path(__file__).resolve().parents[1]
    if args.mode == "references":
        manifest = json.loads((args.output / "manifest.json").read_text(encoding="utf-8"))
        for path, expected in manifest["sources"].items():
            if Path(path).parts[0] == "engine":
                assert digest(repo / path) == expected, f"Measured renderer changed: {path}"
        for path, expected in manifest["project"].items():
            assert digest(args.project / path) == expected, f"Measured project changed: {path}"
        manifest["reference_worker_sha256"] = digest(args.worker)
        manifest["reference_probe_sha256"] = digest(repo / "tests/integration/DirectReferenceProbe.inl")
        manifest["references"] = references(args, manifest["runs"])
        write(args.output / "manifest.json", manifest)
        return
    args.output.mkdir(parents=True, exist_ok=False)
    sources = [p for folder in ("engine", "apps/editor", "tests/integration") for p in (repo / folder).rglob("*") if p.is_file()]
    sources += [Path(__file__)]
    manifest = dict(editor_sha256=digest(args.editor), worker_sha256=digest(args.worker),
                    head=subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=repo, text=True).strip(),
                    sources={str(p.relative_to(repo)): digest(p) for p in sources},
                    project={str(p.relative_to(args.project)): digest(p) for p in args.project.rglob("*") if p.is_file()}, runs=[])
    telemetry = subprocess.run(["nvidia-smi", "--query-gpu=name,driver_version,pstate,memory.total,power.limit,temperature.gpu", "--format=csv"], capture_output=True, text=True)
    manifest["gpu"] = telemetry.stdout.strip()
    manifest["measurement"] = "Release; serial processes; no validation or linear readback in performance runs; three interleaved policy rounds"
    def record(*pos, **kw):
        manifest["runs"].append(motion(args, *pos, **kw))
        write(args.output / "manifest.json", manifest)
    write(args.output / "manifest.json", manifest)
    if args.mode in ("validate", "all"):
        record("validation-v2", "v2", 640, images=True, validation=True)
    if args.mode in ("performance", "all"):
        for width in args.widths:
            for repeat in range(args.rounds):
                order = ("classic", "v1", "v2") if repeat % 2 == 0 else ("v2", "v1", "classic")
                for policy in order:
                    record(f"w{width}-{policy}-{repeat}", policy, width)
    if args.mode in ("quality", "all"):
        for policy in ("classic", "v1", "v2"):
            record("quality-"+policy, policy, 640, images=True)
        manifest["references"] = references(args, manifest["runs"])
        write(args.output / "manifest.json", manifest)


if __name__ == "__main__":
    main()
