"""Cross-process recording/replay acceptance, including Luau and capture when enabled."""

import argparse
from contextlib import contextmanager
import json
from pathlib import Path
import shutil
import subprocess
import sys
import time
from uuid import uuid4

from decker import Client, Command, RpcError, TransportError, guard
from decker.replay import Recorder, replay
from decker.replay_format import ReplayError, canonical, digest, load_recording


def check(ok, message):
    if not ok:
        raise AssertionError(message)


@contextmanager
def server(args, root):
    endpoint = "replay-" + uuid4().hex
    with (root.parent / f"{endpoint}.stdout").open("w+b") as output, (root.parent / f"{endpoint}.stderr").open("w+b") as errors:
        process = subprocess.Popen([args.runner, "--project-root", str(root), "--pipe", endpoint],
                                   stdin=subprocess.DEVNULL, stdout=output, stderr=errors,
                                   creationflags=subprocess.CREATE_NO_WINDOW)
        client = Client(endpoint, args.client)
        try:
            deadline = time.monotonic()+10
            while True:
                try:
                    client.hello(timeout=0.2)
                    break
                except TransportError:
                    check(process.poll() is None and time.monotonic() < deadline, "Runner startup failed")
                    time.sleep(0.02)
            yield client
            client.call("runtime.shutdown")
            check(process.wait(timeout=20) == 0, "Shutdown failed")
            output.seek(0); errors.seek(0)
            check(not output.read(), "Unexpected pipe runner stdout")
            lines = errors.read().decode("utf-8").splitlines()
            allowed = ("[graphics.device] Loader Message: Layer VK_LAYER_AMD_switchable_graphics uses API version 1.3 "
                       "which is older than the application specified API version of 1.4. May cause issues.")
            check(all(args.gpu and line == allowed for line in lines), f"Unexpected diagnostics: {lines}")
            if lines:
                print(f"Known AMD loader warning: {len(lines)} occurrence(s)")
        finally:
            if process.poll() is None:
                process.kill()
                process.wait(timeout=10)


def unloaded(client):
    try:
        client.call("scene.query")
    except RpcError as error:
        check(error.code == -32002, "Wrong no-scene error")
        return
    raise AssertionError("Preflight failure loaded a scene")


def seal(record):
    record["sha256"] = digest({k: v for k, v in record.items() if k != "sha256"})
    return record


def run(args):
    work = Path(args.work_dir) / ("重放-" + uuid4().hex[:10])
    work.mkdir(parents=True)
    base = work / "base"
    if args.script_runner:
        base.mkdir()
        completed = subprocess.run([args.script_runner, "--project-root", str(base), "--script", args.script],
                                   capture_output=True, timeout=30, creationflags=subprocess.CREATE_NO_WINDOW)
        check(completed.returncode == 0 and not completed.stdout and not completed.stderr, "Luau setup failed")
    else:
        shutil.copytree(args.fixtures, base)
    # An explicit external input proves inventory covers files beyond the manifest entries.
    (base / "experiment-input.bin").write_bytes(b"fixed input")
    source, target = work / "source", work / "target"
    shutil.copytree(base, source); shutil.copytree(base, target)
    build = subprocess.check_output([args.runner, "--version"], creationflags=subprocess.CREATE_NO_WINDOW).decode().strip()
    log = work / "experiment.json"
    with server(args, source) as client:
        recorder = Recorder.start(client, source, seed=42, engine_build=build,
                                  context={"experiment": "中文 replay", "camera": "fixture", "units": "metres"})
        query = recorder.call("scene.query").value
        check(len(query["entities"]) >= 3, "Missing initial fixture entities")
        state, entity = query["state"], query["entities"][0]["id"]
        state = recorder.transaction(guard(state), [
            Command("entity.set_name", {"id": entity, "name": "已记录 中文"}),
            Command("entity.set_transform", {"id": entity, "transform": {
                "translation": [recorder.rng.uniform(-0.1, 0.1), -0.125, 0.25],
                "rotation": [0, 0, 0, 1], "scale": [0.5, 0.5, 1]}}),
        ]).value["state"]
        old = guard(state)
        created = recorder.call("entity.create", {"guard": old}).value
        state, created_id = created["state"], created["created_id"]
        state = recorder.call("entity.set_parent", {"guard": guard(state), "id": created_id, "parent": entity}).value["state"]
        try:
            recorder.call("entity.create", {"guard": old})
            raise AssertionError("Stale guard succeeded")
        except RpcError as error:
            check(error.code == -32007, "Wrong stale-guard error")
        try:
            recorder.transaction(guard(state), [Command("entity.set_name", {"id": entity, "name": "must rollback"}),
                                               Command("entity.delete", {"id": "ffffffff-ffff-4fff-8fff-ffffffffffff"})])
            raise AssertionError("Bad transaction succeeded")
        except RpcError:
            check(client.call("entity.get", {"id": entity}).value["name"] == "已记录 中文", "Partial transaction leaked")
        state = recorder.call("history.undo", {"guard": guard(state)}).value
        state = recorder.call("history.redo", {"guard": guard(state)}).value
        state = recorder.call("scene.save", {"guard": guard(state)}).value
        recorder.call("project.save", {"guard": guard(state), "manifest": "project.json"})
        if args.gpu:
            capture = recorder.capture({"guard": guard(state), "output": "recorded.ppm", "width": 64, "height": 64,
                                        "camera": {"eye": [0, 0, -2], "target": [0, 0, 0], "fov_y": 60},
                                        "validation": "required"})
            check(capture.job["result"]["draw_count"] == 3, "Capture did not draw fixture")
        recorder.save(log)
    recording = load_recording(log)
    check(recording["header"]["seed"] == 42 and recording["header"]["context"]["units"] == "metres", "Context missing")
    check(recording["steps"][2]["params"]["id"] == created_id, "Generated ID not materialized")
    with server(args, target) as client:
        # Run the public CLI on a fresh process, not a private test-only path.
        command = [sys.executable, "-m", "decker.replay", str(log), "--pipe", client.endpoint,
                   "--ctl", args.client, "--project-root", str(target), "--engine-build", build]
        result = subprocess.run(command, capture_output=True, timeout=100, creationflags=subprocess.CREATE_NO_WINDOW)
        check(result.returncode == 0 and not result.stderr, f"CLI replay failed: {result.stdout!r} {result.stderr!r}")
        report = json.loads(result.stdout)
        check(report["completed"] == len(recording["steps"]), "Replay skipped steps")
        check(report["final_sha256"] == digest(recording["final"]), "Final logical fingerprint differs")
        check(client.call("entity.get", {"id": created_id}).value["parent"] == entity, "Persistent IDs changed")
        check(client.call("scene.query").value["state"]["document_id"] != recording["header"]["document_id"], "Session reused")
        check((target / "scene.json").read_bytes() == (source / "scene.json").read_bytes(), "Saved scene differs")
        if args.gpu:
            check(len(report["artifacts"]) == 1 and (target / "recorded.ppm").stat().st_size == 12301, "Missing replay artifact")
        (work / "report.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
    rejected = work / "rejected"
    shutil.copytree(base, rejected)
    with server(args, rejected) as client:
        (rejected / "experiment-input.bin").write_bytes(b"changed")
        try:
            replay(client, log, rejected, engine_build=build)
            raise AssertionError("Changed input accepted")
        except ReplayError as error:
            check(error.completed == 0 and error.phase == "preflight", "Input failure was too late")
        unloaded(client)
        (rejected / "experiment-input.bin").write_bytes(b"fixed input")
        drift = json.loads(canonical(recording))
        drift["header"]["commands_sha256"] = "0"*64
        try:
            replay(client, seal(drift), rejected, engine_build=build)
            raise AssertionError("Changed command contract accepted")
        except ReplayError as error:
            check(error.completed == 0, "Contract failure was too late")
        unloaded(client)
        drift = json.loads(canonical(recording))
        drift["steps"][1]["params"]["commands"][0]["params"]["name"] = "deliberate divergence"
        try:
            replay(client, seal(drift), rejected, engine_build=build)
            raise AssertionError("Logical divergence accepted")
        except ReplayError as error:
            check(error.completed == 1 and error.step == 1 and error.phase == "verify", "Wrong divergence location")
        check(client.call("entity.get", {"id": entity}).value["name"] == "deliberate divergence", "Replay claimed rollback")
        check((rejected / "scene.json").read_bytes() == (base / "scene.json").read_bytes(), "Replay ran later save")
        check(not (rejected / "recorded.ppm").exists(), "Replay ran later capture")
    example_source, example_target = work / "demo-source", work / "demo-target"
    shutil.copytree(base, example_source); shutil.copytree(base, example_target)
    example_log = work / "demo.json"
    with server(args, example_source) as client:
        example = Path(__file__).resolve().parents[2] / "examples/automation/record_experiment.py"
        result = subprocess.run([sys.executable, str(example), "--pipe", client.endpoint, "--ctl", args.client,
                                 "--project-root", str(example_source), "--recording", str(example_log),
                                 "--engine-build", build] + (["--capture"] if args.gpu else []), capture_output=True, timeout=60,
                                creationflags=subprocess.CREATE_NO_WINDOW)
        check(result.returncode == 0 and not result.stderr, f"Record example failed: {result.stdout!r} {result.stderr!r}")
    with server(args, example_target) as client:
        report = replay(client, example_log, example_target, engine_build=build)
        check(report.completed == (4 if args.gpu else 3), "Example replay incomplete")
    print(f"Recording/replay passed ({'GPU' if args.gpu else 'CPU'}, Luau seed={bool(args.script_runner)}): {work}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    for name in ("runner", "client", "fixtures", "work-dir"):
        parser.add_argument("--" + name, required=True)
    parser.add_argument("--script-runner")
    parser.add_argument("--script")
    parser.add_argument("--gpu", action="store_true")
    run(parser.parse_args())
