"""M10.4 command/IPC/script acceptance. Artifacts are retained for inspection."""
import argparse
import json
from pathlib import Path
import subprocess
import sys
import time
from uuid import uuid4

from decker import RpcError, guard
from PythonClientTest import server


def rejected(client, method, params):
    try:
        client.call(method, params)
    except RpcError:
        return
    raise AssertionError(f"Unexpected success: {method} {params}")


def read(root, name, file):
    return json.loads((root / name / file).read_text(encoding="utf-8"))


def main():
    parser = argparse.ArgumentParser()
    for option in ("runner", "client", "example", "luau", "work-dir"):
        parser.add_argument("--" + option, required=True)
    args = parser.parse_args()
    args.gpu = True  # server fixture permits only the documented optional loader warning
    args.pipe_timeout_ms = 60000
    root = Path(args.work_dir).resolve() / uuid4().hex
    root.mkdir(parents=True)
    for arguments in (["--pipe", "unused", "--pipe-timeout-ms", "0"],
                      ["--pipe", "unused", "--pipe-timeout-ms", "60001"],
                      ["--pipe", "unused", "--pipe-timeout-ms", "5x"],
                      ["--pipe", "unused", "--pipe-timeout-ms", "5", "--pipe-timeout-ms", "6"],
                      ["--stdio", "--pipe-timeout-ms", "5"]):
        bad = subprocess.run([args.runner, "--project-root", str(root), *arguments],
                             capture_output=True, timeout=10)
        assert bad.returncode == 2 and not bad.stdout, bad
    endpoint = "dk-experiment-" + uuid4().hex
    with server(args, root, endpoint) as client:
        client.timeout = 60
        caps = client.call("runtime.capabilities").value["simulation"]
        assert caps["gpu"] and caps["experiment_export"]
        description = client.call("commands.describe", {"name": "simulation.export"}).value
        assert description["effect"] == "external" and description["undoable"] is False
        state = client.call("scene.new", {"name": "Experiment EditWorld"}).value
        state = client.call("entity.create", {"guard": guard(state)}).value["state"]
        before = client.call("scene.query").value
        history = client.call("history.status").value
        start = {"guard": guard(state), "solver": "xpbd_gpu", "paused": True,
                 "fixed_dt_ns": 10000000, "cloth": {"seed": 42}}
        rejected(client, "simulation.start", {**start, "max_catch_up_steps": 9})
        try:
            run = client.call("simulation.start", start).value["run"]
        except RpcError as error:
            if (error.data.get("engine_name") in ("not_found", "not_supported")
                    and "simulation.gpu.device" in error.data.get("context", [])):
                print(f"GPU unavailable; experiment skipped: {error}")
                raise SystemExit(77)
            raise
        identity = {"run_id": run["run_id"]}
        assert run["metrics"] is None and run["solver"] == "xpbd_gpu"
        time.sleep(.1)
        assert client.call("simulation.query").value["run"]["steps"] == 0
        export = {**identity, "expected_steps": 0, "output": "initial"}
        client.call("simulation.export", export)
        assert client.call("simulation.query").value["run"]["steps"] == 0
        page = client.call("simulation.particles", {**identity, "offset": 60, "limit": 8}).value
        assert page["steps"] == 0 and len(page["particles"]) == 4 and not page["has_more"]
        initial_bytes = {p.name: p.read_bytes() for p in (root / "initial").iterdir()}
        rejected(client, "simulation.step", {**identity, "count": 9})
        rejected(client, "simulation.step", {**identity, "run_id": str(uuid4())})
        (root / "existing.txt").write_text("preserve", encoding="utf-8")
        (root / "empty").mkdir()
        for output in ("initial", "existing.txt", "empty", "../escape", ".decker/result", "missing/result", str(root / "absolute")):
            rejected(client, "simulation.export", {**export, "output": output})
        rejected(client, "simulation.export", {**export, "output": "bad-steps", "expected_steps": 1})
        rejected(client, "simulation.export", {**export, "output": "bad-size", "width": 0})
        assert initial_bytes == {p.name: p.read_bytes() for p in (root / "initial").iterdir()}
        assert (root / "existing.txt").read_text() == "preserve"
        assert not list(root.glob(".dk-experiment-*"))
        # A final partial batch proves exact N rather than rounded multiples of eight.
        for first in range(0, 300, 8):
            run = client.call("simulation.step", {**identity, "count": min(8, 300-first)}).value["run"]
        assert run["steps"] == 300 and run["simulated_time_ns"] == 3000000000
        exported = client.call("simulation.export", {**export, "output": "gpu", "expected_steps": 300}).value
        assert len(exported["files"]) == 5 and exported["steps"] == 300
        time.sleep(.1)
        assert client.call("simulation.query").value["run"]["steps"] == 300
        # Edit continues independently; stopping must preserve these latest edits/history.
        state = client.call("entity.set_name", {"guard": guard(state), "id": before["entities"][0]["id"], "name": "latest"}).value["state"]
        latest = client.call("scene.query").value
        latest_history = client.call("history.status").value
        assert latest != before and latest_history != history
        client.call("simulation.stop", identity)
        assert client.call("scene.query").value == latest
        assert client.call("history.status").value == latest_history
        for name, solver in (("repeat", "xpbd_gpu"), ("cpu", "xpbd_cpu")):
            replay = subprocess.run([sys.executable, args.example, "--pipe", endpoint, "--ctl", args.client,
                "--config", str(root / "gpu/config.json"), "--output", name, "--solver", solver],
                capture_output=True, text=True, encoding="utf-8", timeout=120)
            assert replay.returncode == 0, replay.stdout + replay.stderr
            assert json.loads(replay.stdout)["steps"] == 300
        for file in ("config.json", "metrics.json", "particles.json", "image.ppm"):
            assert (root / "gpu" / file).read_bytes() == (root / "repeat" / file).read_bytes(), file
        assert client.call("scene.query").value == latest
        assert client.call("history.status").value == latest_history
        assert client.call("simulation.query").value["mode"] == "edit"
        # Running snapshots cannot be mistaken for exact-step experiments.
        run = client.call("simulation.start", {**start, "guard": guard(state)}).value["run"]
        identity = {"run_id": run["run_id"]}
        client.call("simulation.resume", identity)
        rejected(client, "simulation.export", {**identity, "output": "running", "expected_steps": 0})
        client.call("simulation.pause", identity)
        paused = client.call("simulation.query").value
        time.sleep(.1)
        assert client.call("simulation.query").value == paused
        client.call("simulation.stop", identity)
    gpu, cpu = (read(root, n, "particles.json") for n in ("gpu", "cpu"))
    errors = {field: max(abs(a-b) for gp, cp in zip(gpu, cpu) for a,b in zip(gp[field],cp[field]))
              for field in ("position", "velocity")}
    assert len(gpu) == len(cpu) == 64 and errors["position"] < .002 and errors["velocity"] < .02, errors
    metrics = read(root, "gpu", "metrics.json")
    assert metrics["steps"] == 300 and metrics["simulated_time_ns"] == 3000000000
    assert metrics["metrics"]["max_constraint_error"] < .002
    assert metrics["metrics"]["max_penetration"] == 0 and metrics["metrics"]["max_pin_displacement"] == 0
    image = (root / "gpu/image.ppm").read_bytes().split(b"\n", 3)[3]
    assert len(image) == 640*480*3 and len(set(image)) > 10
    assert (root / "gpu/image.ppm").read_bytes() != (root / "initial/image.ppm").read_bytes()
    # Luau uses exactly the same command/service path in a fresh process.
    luau_root = root / "luau"
    luau_root.mkdir()
    result = subprocess.run([args.runner, "--project-root", str(luau_root), "--script", args.luau,
        "--script-timeout-ms", "120000"], capture_output=True, text=True, encoding="utf-8", timeout=150)
    assert result.returncode == 0, result.stdout + result.stderr
    assert not result.stdout
    allowed = ("[graphics.device] Loader Message: Layer VK_LAYER_AMD_switchable_graphics uses API version 1.3 "
               "which is older than the application specified API version of 1.4. May cause issues.")
    assert all(line == allowed for line in result.stderr.splitlines()), result.stderr
    for file in ("config.json", "metrics.json", "particles.json", "image.ppm"):
        assert (root / "gpu" / file).read_bytes() == (luau_root / "luau-experiment" / file).read_bytes(), file
    (root / "comparison.json").write_text(json.dumps(errors, indent=2), encoding="utf-8")
    print(f"Exact 300 steps, repeat bytes, CPU/GPU tolerance, failure protection, Edit preservation, Python/Luau passed: {root}; {errors}")


if __name__ == "__main__":
    main()
