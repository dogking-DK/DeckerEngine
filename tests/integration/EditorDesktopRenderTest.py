"""Normal camera/window and default Sponza: real path/atmosphere GUI transitions."""
import argparse
import json
from pathlib import Path
import shutil
import subprocess
import time
from uuid import uuid4


def main():
    parser = argparse.ArgumentParser()
    for name in ("editor", "project", "work-dir"):
        parser.add_argument("--" + name, type=Path, required=True)
    args = parser.parse_args()
    directory = args.work_dir / ("desktop-render-" + uuid4().hex)
    root = directory / "project"
    shutil.copytree(args.project, root)
    (root / ".dk-editor-smoke").touch()
    evidence = root / ".dk-path"
    report = {"status": "failed", "checkpoints": []}
    with (directory / "stdout.log").open("wb") as out, (directory / "stderr.log").open("wb") as err:
        process = subprocess.Popen([str(args.editor), "--background", "--root", str(root),
                                    "--validation", "--desktop-render-smoke", "--screenshot", str(directory / "workbench.ppm")],
                                   stdin=subprocess.DEVNULL, stdout=out, stderr=err,
                                   creationflags=subprocess.CREATE_NO_WINDOW)
        try:
            for name in ("path", "atmosphere", "clouds", "returned"):
                deadline = time.monotonic() + 150
                while True:
                    assert process.poll() is None, (directory / "stderr.log").read_text()
                    try:
                        value = json.loads((evidence / (name + ".json")).read_text())
                        break
                    except (FileNotFoundError, PermissionError):
                        assert time.monotonic() < deadline, f"Timed out at {name}: {directory}"
                        time.sleep(.05)
                assert value["width"] > 320 and value["height"] > 240, value
                raster = name in ("atmosphere", "clouds")
                assert value["spp"] == (0 if raster else 1), value
                assert value["path_mode"] != raster, value
                ppm = (evidence / (name + ".ppm")).read_bytes()
                pixels = ppm.split(b"\n", 3)[3]
                assert len(pixels) == value["width"] * value["height"] * 3
                assert len(set(pixels)) > 32, "Uniform/empty viewport"
                report["checkpoints"].append(value)
                (evidence / (name + ".continue")).touch()
            assert report["checkpoints"][1]["revision"] == report["checkpoints"][0]["revision"] + 1
            assert report["checkpoints"][1]["pixels"] != report["checkpoints"][0]["pixels"]
            assert report["checkpoints"][2]["revision"] == report["checkpoints"][1]["revision"] + 1
            assert process.wait(timeout=45) == 0, (directory / "stderr.log").read_text()
            log = (directory / "stdout.log").read_text()
            assert "validation errors=0 warnings=0 liveAllocations=0" in log, log
            report["status"] = "passed"
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()
            (directory / "report.json").write_text(json.dumps(report, indent=2))
    print(directory)


if __name__ == "__main__":
    main()
