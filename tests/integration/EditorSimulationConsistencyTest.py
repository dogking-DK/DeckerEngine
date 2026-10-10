"""M12.4: real GUI and SDK/IPC alternate ownership, then a Python script replays the same export."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys
import time
from uuid import uuid4

from decker import Client, RpcError, guard


def read(path):
    return json.loads(path.read_text(encoding="utf-8"))


def hashes(directory):
    return {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in directory.iterdir()}


def rejected(client, method, params, code="conflict"):
    try:
        client.call(method, params)
    except RpcError as error:
        assert error.data["engine_name"] == code, error
    else:
        raise AssertionError("Unexpected success: " + method)


def main():
    parser = argparse.ArgumentParser()
    for name in ("editor", "client", "fixtures", "example", "work-dir"):
        parser.add_argument("--" + name, type=Path, required=True)
    args = parser.parse_args()
    directory = args.work_dir / ("simulation-consistency-" + uuid4().hex)
    root = directory / "project"
    shutil.copytree(args.fixtures, root)
    (root / ".dk-editor-smoke").touch()
    original_scene = (root / "scene.json").read_bytes()
    evidence = root / ".dk-simulation-consistency"
    endpoint = "dk-simulation-consistency-" + uuid4().hex
    report = {"status": "failed", "backends": [], "checkpoints": []}
    command = [args.editor, "--root", root, "--pipe", endpoint, "--fixture-camera", "--validation",
               "--simulation-consistency-smoke", "--screenshot", directory / "workbench.ppm"]
    with (directory / "stdout.log").open("wb") as out, (directory / "stderr.log").open("wb") as err:
        process = subprocess.Popen([str(x) for x in command], stdin=subprocess.DEVNULL, stdout=out, stderr=err,
                                   creationflags=subprocess.CREATE_NO_WINDOW)
        client = Client(endpoint, args.client, timeout=30)

        def checkpoint(name):
            path = evidence / (name + ".json")
            end = time.monotonic() + 45
            while not path.exists():
                if process.poll() == 77:
                    raise SystemExit(77)
                if process.poll() is not None or time.monotonic() >= end:
                    raise RuntimeError("Editor did not present " + name + ": " + (directory / "stderr.log").read_text())
                time.sleep(.01)
            value = read(path)
            assert value["checkpoint"] == name and value["viewport"]["width"] > 0
            scene, history = client.call("scene.query").value, client.call("history.status").value
            edit = value["edit"]
            assert edit["document_id"] == scene["state"]["document_id"] and edit["revision"] == scene["state"]["revision"]
            assert edit["dirty"] == scene["state"]["dirty"]
            assert edit["names"] == {e["id"]: e["name"] for e in scene["entities"]}
            assert edit["undo_count"] == history["undo_count"] and edit["redo_count"] == history["redo_count"]
            report["checkpoints"].append(value)
            return value

        def advance(name):
            (evidence / (name + ".continue")).touch()

        def settled(steps=None):
            end = time.monotonic() + 10
            while True:
                value = client.call("simulation.query").value
                assert value["run"] and value["run"]["fault"] is None, value
                if value["mode"] == "paused":
                    if steps is not None:
                        assert value["run"]["steps"] == steps, value
                    return value["run"]
                if time.monotonic() >= end:
                    raise TimeoutError("Simulation did not settle: " + str(value))
                time.sleep(.002)

        try:
            for backend in ("cpu", "gpu"):
                name = lambda stage: backend + "-" + stage
                checkpoint(name("initial"))
                before, old_history = client.call("scene.query").value, client.call("history.status").value
                advance(name("initial"))
                prepared = checkpoint(name("prepared"))
                run = settled(0)
                key = {"run_id": run["run_id"]}
                assert prepared["simulation"]["run_id"] == key["run_id"]
                assert run["solver"] == "xpbd_" + backend and run["source"] == before["state"]
                advance(name("prepared"))
                stepped = checkpoint(name("stepped"))
                assert stepped["simulation"]["steps"] == 1 and settled(1)["metrics_sample"] is None
                advance(name("stepped"))
                sampled = checkpoint(name("sampled"))
                assert sampled["simulation"]["sample_steps"] == 1
                # The GUI contributes one step; IPC supplies exactly the remaining 299, with no retries.
                for first in range(1, 300, 8):
                    count = min(8, 300-first)
                    client.call("simulation.step", {**key, "count": count})
                    run = settled(first+count)
                    assert run["run_id"] == key["run_id"] and run["metrics_sample"]["steps"] == 1
                entity = before["entities"][0]["id"]
                client.call("entity.set_name", {"guard": guard(before["state"]), "id": entity, "name": "M12.4 " + backend})
                latest, latest_history = client.call("scene.query").value, client.call("history.status").value
                assert latest_history["undo_count"] == old_history["undo_count"] + 1
                rejected(client, "simulation.sample", {**key, "expected_steps": 1})
                rejected(client, "simulation.step", {"run_id": str(uuid4()), "count": 1})
                advance(name("sampled"))
                advanced = checkpoint(name("advanced"))
                assert advanced["simulation"]["steps"] == advanced["viewport"]["steps"] == 300
                assert advanced["simulation"]["sample_steps"] == 1
                advance(name("advanced"))
                exported = checkpoint(name("exported"))
                artifact = root / exported["export"]["output"]
                config, metrics, provenance = (read(artifact / f) for f in ("config.json", "metrics.json", "provenance.json"))
                assert config["steps"] == metrics["steps"] == provenance["steps"] == 300
                assert provenance["run_id"] == key["run_id"] and provenance["revision"] == before["state"]["revision"]
                assert metrics["simulated_time_ns"] == 300 * config["fixed_dt_ns"] == 3000000000
                assert client.call("simulation.sample", {**key, "expected_steps": 300}).value["run"]["metrics_sample"]["metrics"] == metrics["metrics"]
                valid_files = hashes(artifact)
                rejected(client, "simulation.export", {**key, "expected_steps": 300, "output": exported["export"]["output"]})
                rejected(client, "simulation.export", {**key, "expected_steps": 301, "output": "wrong-" + backend})
                assert hashes(artifact) == valid_files and not (root / ("wrong-" + backend)).exists()
                advance(name("exported"))
                running = checkpoint(name("running"))
                assert running["simulation"]["steps"] > 300
                client.call("simulation.pause", key)
                paused = settled()
                assert paused["steps"] > 300 and paused["metrics_sample"]["steps"] == 300
                advance(name("running"))
                shown_pause = checkpoint(name("paused"))
                assert shown_pause["viewport"]["steps"] == paused["steps"]
                time.sleep(.05)
                assert client.call("simulation.query").value["run"] == paused
                advance(name("paused"))
                checkpoint(name("stopped"))
                assert client.call("simulation.query").value == {"mode": "edit", "run": None}
                assert client.call("scene.query").value == latest and client.call("history.status").value == latest_history
                edited_entities = latest["entities"]
                client.call("history.undo", {"guard": guard(latest["state"])})
                undone = client.call("scene.query").value
                assert undone["entities"] == before["entities"]
                client.call("history.redo", {"guard": guard(undone["state"])})
                latest = client.call("scene.query").value
                assert latest["entities"] == edited_entities
                # Reuse the shipped Python script against this same editor, while GUI remains alive.
                replay = subprocess.run([sys.executable, str(args.example), "--pipe", endpoint, "--ctl", str(args.client),
                    "--config", str(artifact / "config.json"), "--output", "replay-" + backend],
                    capture_output=True, text=True, encoding="utf-8", timeout=90, creationflags=subprocess.CREATE_NO_WINDOW)
                (directory / (backend + "-replay.stdout.json")).write_text(replay.stdout, encoding="utf-8")
                (directory / (backend + "-replay.stderr.log")).write_text(replay.stderr, encoding="utf-8")
                assert replay.returncode == 0 and not replay.stderr, replay.stdout + replay.stderr
                replay_receipt = json.loads(replay.stdout)
                assert replay_receipt["run_id"] != key["run_id"] and replay_receipt["steps"] == 300
                replay_root = root / ("replay-" + backend)
                for file in ("config.json", "metrics.json", "particles.json", "image.ppm"):
                    assert (artifact / file).read_bytes() == (replay_root / file).read_bytes(), (backend, file)
                assert read(replay_root / "provenance.json")["run_id"] == replay_receipt["run_id"]
                assert client.call("scene.query").value == latest and client.call("history.status").value == latest_history
                rejected(client, "simulation.step", key, "invalid_state")
                advance(name("stopped"))
                checkpoint(name("replayed"))
                report["backends"].append({"backend": backend, "run_id": key["run_id"], "replay_run_id": replay_receipt["run_id"],
                    "export": str(artifact), "files": valid_files, "paused_steps": paused["steps"]})
                advance(name("replayed"))
            assert process.wait(timeout=15) == 0
            assert (root / "scene.json").read_bytes() == original_scene
            assert not list(root.glob(".dk-experiment-*"))
            assert (directory / "workbench.ppm").stat().st_size > 100000
        finally:
            if process.poll() is None:
                process.kill()
                process.wait(timeout=10)
            (directory / "report.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
    output = (directory / "stdout.log").read_text(encoding="utf-8")
    counts = re.search(r"validation errors=(\d+) warnings=(\d+) liveAllocations=(\d+) knownLoaderWarnings=(\d+)", output)
    assert counts and counts[1] == counts[3] == "0" and counts[2] == counts[4], output
    allowed = "Loader Message: Layer VK_LAYER_AMD_switchable_graphics uses API version 1.3 which is older than the application specified API version of 1.4. May cause issues."
    assert all(line == allowed for line in (directory / "stderr.log").read_text().splitlines())
    report["status"] = "passed"
    (directory / "report.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
    print(f"CPU/GPU GUI+IPC exact 300, paused pixels, export/replay, latest Edit/undo and validation/lifetime passed: {directory}")


if __name__ == "__main__":
    main()
