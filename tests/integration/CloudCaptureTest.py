"""M15.3 real runner: frozen cloud captures, discovery, provenance and publication failure."""
import argparse
import copy
import json
from pathlib import Path
import queue
import re
import shutil
import subprocess
import threading
import time
from uuid import uuid4


def main():
    parser = argparse.ArgumentParser()
    for name in ("runner", "fixtures", "work-dir"):
        parser.add_argument("--" + name, type=Path, required=True)
    args = parser.parse_args()
    root = args.work_dir / ("cloud-capture-" + uuid4().hex)
    shutil.copytree(args.fixtures, root)
    for asset in (root / "assets").glob("*.gltf"):
        gltf = json.loads(asset.read_text())
        for material in gltf["materials"]:
            material["doubleSided"] = True
        asset.write_text(json.dumps(gltf), encoding="utf-8")
    disk = (root / "scene.json").read_bytes()
    report = {"status": "failed", "captures": []}
    with (root / "stderr.log").open("wb") as stderr:
        process = subprocess.Popen([str(args.runner), "--project-root", str(root), "--stdio"],
                                   stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=stderr,
                                   text=True, encoding="utf-8", creationflags=subprocess.CREATE_NO_WINDOW)
        responses = queue.Queue()
        def read_responses():
            for line in process.stdout:
                responses.put(line)
            responses.put(None)
        reader = threading.Thread(target=read_responses, daemon=True)
        reader.start()
        request_id = 0

        def call(method, params=None):
            nonlocal request_id
            request_id += 1
            process.stdin.write(json.dumps({"jsonrpc": "2.0", "id": request_id, "method": method, "params": params or {}}) + "\n")
            process.stdin.flush()
            line = responses.get(timeout=20)
            assert line is not None, method + " unexpected EOF"
            response = json.loads(line)
            assert response["id"] == request_id and "result" in response, response
            assert response["result"]["status"] == "succeeded", response
            return response["result"]["value"]

        def guard(state):
            return {key: state[key] for key in ("document_id", "revision")}

        def edit(environment):
            state = call("environment.query")["state"]
            return call("environment.set", {"guard": guard(state), "environment": environment})["state"]

        def capture(state, name, side=16):
            return call("render.capture", {"guard": guard(state), "output": name, "width": side, "height": side,
                        "camera": {"eye": [0, 0, -2], "target": [0, 0, 0], "fov_y": 60}, "validation": "required"})

        def wait(ticket, success=True):
            deadline = time.monotonic() + 90
            while time.monotonic() < deadline:
                waited = call("jobs.wait", {"id": ticket["job_id"], "timeout_ms": 1000})
                if not waited["timed_out"]:
                    job = waited["job"]
                    break
            else:
                raise TimeoutError("cloud capture did not finish")
            report["captures"].append(job)
            assert job["state"] == ("succeeded" if success else "failed"), job
            if success:
                result = job["result"]
                expected = copy.deepcopy(ticket["provenance"])
                expected["shader_fingerprint"] = result["provenance"]["shader_fingerprint"]
                assert result["provenance"] == expected
                assert expected["time_seconds"] == 0
                assert re.fullmatch(r"fnv1a64:[0-9a-f]{16}", expected["shader_fingerprint"])
                for key in ("document_id", "scene_id", "revision", "width", "height", "frame", "output"):
                    assert result[key] == ticket[key], key
                image = (root / result["output"]).read_bytes()
                assert image.startswith(f'P6\n{result["width"]} {result["height"]}\n255\n'.encode())
            return job

        try:
            commands = call("commands.list")
            found = [item for item in commands if item["name"] == "render.capture"]
            assert len(found) == 1 and found[0]["effect"] == "external" and not found[0]["undoable"]
            descriptor = call("commands.describe", {"name": "render.capture"})
            assert not descriptor["parameters"]["additionalProperties"]
            assert "time_seconds" not in descriptor["parameters"]["properties"]
            quality = descriptor["result"]["properties"]["provenance"]["properties"]["quality"]["enum"]
            assert set(quality) == {"cloud_reference_v1", "atmosphere_v1", "unlit_v1"}
            (root / "capture.schema.json").write_text(json.dumps(descriptor, indent=2), encoding="utf-8")
            call("scene.load", {"manifest": "project.json"})
            environment = call("environment.query")["environment"]
            environment["enabled"] = True
            environment["clouds"].update(enabled=True, coverage=1)
            environment["atmosphere"]["planet_center_m"][1] = -environment["atmosphere"]["ground_radius_m"] - 2200
            state = edit(environment)
            history = call("history.status")
            first = capture(state, "first.ppm")
            assert first["provenance"]["quality"] == "cloud_reference_v1"
            wait(first)
            assert call("scene.query")["state"] == state and call("history.status") == history
            frozen = capture(state, "frozen.ppm")
            zero = copy.deepcopy(environment)
            zero["clouds"]["coverage"] = 0
            state = edit(zero)
            wait(frozen)
            assert (root / "first.ppm").read_bytes() == (root / "frozen.ppm").read_bytes()
            clear = capture(state, "clear.ppm")
            assert clear["provenance"]["quality"] == "atmosphere_v1"
            wait(clear)
            assert (root / "clear.ppm").read_bytes() != (root / "first.ppm").read_bytes()
            state = edit(environment)
            repeat = capture(state, "repeat.ppm")
            wait(repeat)
            assert (root / "repeat.ppm").read_bytes() == (root / "first.ppm").read_bytes()
            old = (root / "first.ppm").read_bytes()
            failed = wait(capture(state, "first.ppm", 65), success=False)
            assert "4096" in failed["error"]["message"], failed
            assert (root / "first.ppm").read_bytes() == old
            environment["enabled"] = False
            state = edit(environment)
            cloud_only = capture(state, "cloud-only.ppm")
            assert cloud_only["provenance"]["quality"] == "cloud_reference_v1"
            wait(cloud_only)
            assert (root / "scene.json").read_bytes() == disk
            call("runtime.shutdown")
            process.wait(timeout=15)
            reader.join(timeout=5)
            assert process.returncode == 0 and responses.get(timeout=5) is None
            assert (root / "stderr.log").read_bytes() == b""
            report["status"] = "passed"
        finally:
            if process.poll() is None:
                process.kill()
                process.wait(timeout=10)
            process.stdin.close()
            reader.join(timeout=5)
            process.stdout.close()
            (root / "report.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
    print("cloud capture passed: frozen scene/time, quality/schema, zero fallback, publication failure; " + str(root))


if __name__ == "__main__":
    main()
