"""Normal camera/window and default Sponza: real path/atmosphere GUI transitions."""
import argparse
import json
from pathlib import Path
import shutil
import subprocess
import time
from uuid import uuid4
import sys
sys.path.insert(0, str(Path(__file__).parents[2] / "sdk/python"))
from decker import Client, guard


def main():
    parser = argparse.ArgumentParser()
    for name in ("editor", "client", "project", "work-dir"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--performance", action="store_true")
    args = parser.parse_args()
    directory = args.work_dir / ("desktop-render-" + uuid4().hex)
    root = directory / "project"
    shutil.copytree(args.project, root)
    (root / ".dk-editor-smoke").touch()
    evidence = root / ".dk-path"
    endpoint = "dk-sponza-" + uuid4().hex
    client = Client(endpoint, args.client, timeout=30)
    report = {"status": "failed", "validation": not args.performance, "checkpoints": [], "commands": []}
    with (directory / "stdout.log").open("wb") as out, (directory / "stderr.log").open("wb") as err:
        process = subprocess.Popen([str(args.editor), "--background", "--root", str(root),
                                    "--pipe", endpoint, "--no-validation" if args.performance else "--validation",
                                    "--desktop-render-smoke", "--screenshot", str(directory / "workbench.ppm")],
                                   stdin=subprocess.DEVNULL, stdout=out, stderr=err,
                                   creationflags=subprocess.CREATE_NO_WINDOW)
        try:
            for name in ("path", "atmosphere", "air_motion", "clouds_enabled", "clouds", "cloud_motion", "rejected", "recovered", "underground", "camera_recovered", "returned", "media_air", "media_air_motion", "media_cloud", "media_cloud_motion"):
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
                raster = name not in ("path", "returned", "media_air", "media_air_motion", "media_cloud", "media_cloud_motion")
                assert value["spp"] == (0 if raster else 1), value
                assert value["path_mode"] != raster, value
                ppm = (evidence / (name + ".ppm")).read_bytes()
                pixels = ppm.split(b"\n", 3)[3]
                assert len(pixels) == value["width"] * value["height"] * 3
                assert len(set(pixels)) > 32, "Uniform/empty viewport"
                report["checkpoints"].append(value)
                if name in ("air_motion", "cloud_motion", "rejected"):
                    query = client.call("environment.query").value
                    if name == "air_motion":
                        query["environment"]["clouds"]["enabled"] = True
                    elif name == "cloud_motion":
                        import copy
                        saved_environment = copy.deepcopy(query["environment"])
                        query["environment"]["clouds"]["bottom_height_m"] = 1
                    else:
                        assert "below-cloud covered region" in value["error"], value
                        assert value["pixels"] == report["checkpoints"][-2]["pixels"], "Failure replaced last image"
                        assert value["revision"] < value["scene_revision"], "Old image mislabeled current"
                        query["environment"] = saved_environment
                    response = client.call("environment.set", {"guard": guard(query["state"]), "environment": query["environment"]})
                    report["commands"].append({"after": name, "response": response.value})
                if name in ("air_motion", "cloud_motion"):
                    assert value["motion_updates"] >= 2, value
                if name in ("recovered", "camera_recovered"):
                    assert not value["error"] and value["revision"] == value["scene_revision"], value
                if name == "underground":
                    assert "below the atmosphere ground" in value["error"], value
                if name in ("returned", "media_air_motion"):
                    preset = client.call("environment.preset", dict(name="clear" if name == "returned" else "sparse")).value
                    state = client.call("scene.query").value["state"]
                    client.call("environment.set", dict(guard=guard(state), environment=preset["environment"]))
                if name in ("media_air_motion", "media_cloud_motion"):
                    assert value["motion_updates"] >= 16, value
                    assert len(value["motion_samples"]) >= 16
                    assert all(max(s["width"], s["height"]) <= 192 for s in value["motion_samples"]), value
                    report.setdefault("media_latency_ms", []).extend(s["latency_ms"] for s in value["motion_samples"])
                (evidence / (name + ".continue")).touch()
            assert report["checkpoints"][1]["revision"] == report["checkpoints"][0]["revision"] + 1
            assert report["checkpoints"][1]["pixels"] != report["checkpoints"][0]["pixels"]
            assert report["checkpoints"][3]["revision"] == report["checkpoints"][1]["revision"] + 1
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
