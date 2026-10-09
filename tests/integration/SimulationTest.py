"""Exercise fixed-step control and idle owner pumping through real Windows hosts."""
import argparse
from contextlib import contextmanager
import json
from pathlib import Path
import queue
import subprocess
import threading
import time
from uuid import uuid4

from decker import Client, TransportError, guard


@contextmanager
def host(args, root):
    endpoint = "dk-simulation-" + uuid4().hex
    mode = ["--stdio"] if args.mode == "stdio" else ["--pipe", endpoint]
    with (root / "stderr.log").open("w+b") as errors:
        process = subprocess.Popen([args.runner, "--project-root", str(root), *mode],
                                   stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=errors,
                                   text=True, encoding="utf-8", creationflags=subprocess.CREATE_NO_WINDOW)
        lines = queue.Queue()

        def read():
            for line in process.stdout:
                lines.put(line)

        reader = threading.Thread(target=read)
        reader.start()
        try:
            if args.mode == "stdio":
                request_id = 0

                def call(method, params=None):
                    nonlocal request_id
                    request_id += 1
                    request = {"jsonrpc": "2.0", "id": request_id, "method": method, "params": params or {}}
                    process.stdin.write(json.dumps(request) + "\n")
                    process.stdin.flush()
                    reply = json.loads(lines.get(timeout=10))
                    assert reply["id"] == request_id and "error" not in reply, reply
                    return reply["result"]["value"]
            else:
                client = Client(endpoint, args.client)
                deadline = time.monotonic() + 10
                while True:
                    try:
                        client.hello(timeout=0.2)
                        break
                    except TransportError:
                        assert process.poll() is None, "Runner exited during startup"
                        if time.monotonic() >= deadline:
                            raise
                        time.sleep(0.02)

                def call(method, params=None):
                    return client.call(method, params).value

            yield call
            call("runtime.shutdown")
            assert process.wait(timeout=10) == 0
            reader.join(timeout=5)
            assert not reader.is_alive() and lines.empty(), "Unexpected stdout"
            errors.seek(0)
            assert not errors.read(), "Unexpected diagnostics"
        finally:
            if process.poll() is None:
                process.kill()
                process.wait(timeout=10)
            reader.join(timeout=5)
            process.stdin.close()
            process.stdout.close()


def exercise(call, root):
    state = call("scene.new", {"name": "Simulation integration"})
    state = call("entity.create", {"guard": guard(state)})["state"]
    before = call("scene.query")
    history = call("history.status")
    run = call("simulation.start", {"guard": guard(state), "fixed_dt_ns": 10000000,
                                    "max_catch_up_steps": 1, "paused": True})["run"]
    run_id = {"run_id": run["run_id"]}
    time.sleep(0.15)
    assert call("simulation.query")["run"]["steps"] == 0
    stepped = call("simulation.step", {**run_id, "count": 123})["run"]
    assert stepped["steps"] == 123 and stepped["simulated_time_ns"] == 1230000000
    call("simulation.resume", run_id)
    # If idle host pumping is absent, dispatch can add only one catch-up tick.
    time.sleep(0.5)
    paused = call("simulation.pause", run_id)
    assert paused["run"]["steps"] > 128, paused
    time.sleep(0.15)
    assert call("simulation.query") == paused, "Paused clock advanced"
    call("simulation.stop", run_id)
    assert call("scene.query") == before
    assert call("history.status") == history
    assert not (root / "scene.json").exists(), "Simulation saved the editing scene"
    methods = {d["name"]: d for d in call("commands.list")}
    for name in ("start", "pause", "resume", "step", "stop", "query"):
        method = "simulation." + name
        assert methods[method]["effect"] == ("query" if name == "query" else "control")
        assert not methods[method]["undoable"]
        description = call("commands.describe", {"name": method})
        assert set(description["result"]["properties"]) == {"mode", "run"}
        if name == "start":
            assert description["parameters"]["required"] == ["guard"]
            assert description["parameters"]["properties"]["fixed_dt_ns"]["minimum"] == 1000000
        elif name != "query":
            assert description["parameters"]["required"] == ["run_id"]
    # Shutdown with a live running PlayWorld must still flush and terminate promptly.
    xpbd(call, state, root)
    call("simulation.start", {"guard": guard(state)})


def xpbd(call, state, root):
    assert call("runtime.capabilities")["simulation"]["solver"] == "xpbd_cpu"
    before = call("scene.query")
    results = []
    for _ in range(2):
        run = call("simulation.start", {"guard": guard(state), "solver": "xpbd_cpu", "paused": True,
                                        "fixed_dt_ns": 10000000, "cloth": {"seed": 42}})["run"]
        key = {"run_id": run["run_id"]}
        initial = call("simulation.particles", key)["particles"]
        final = call("simulation.step", {**key, "count": 300})["run"]
        assert final["steps"] == 300 and final["simulated_time_ns"] == 3000000000
        metrics = final["metrics"]
        assert metrics["particle_count"] == 64 and metrics["constraint_count"] == 210
        assert metrics["max_pin_displacement"] == 0 and metrics["max_penetration"] <= 1e-6
        assert metrics["max_relative_error"] < 0.08
        particles = []
        for offset in range(0, 64, 16):
            page = call("simulation.particles", {**key, "offset": offset, "limit": 16})
            assert page["steps"] == 300 and page["run_id"] == run["run_id"]
            assert page["has_more"] == (offset < 48)
            particles.extend(page["particles"])
        assert particles[:8] == initial[:8], "Pins moved"
        assert particles[-1]["position"][1] < initial[-1]["position"][1] - 0.1
        assert call("simulation.query")["run"] == final
        description = call("commands.describe", {"name": "simulation.particles"})
        assert description["effect"] == "query" and not description["undoable"]
        call("simulation.stop", key)
        assert call("scene.query") == before
        results.append({"cloth": final["cloth"], "metrics": metrics, "particles": particles,
                        "steps": final["steps"], "simulated_time_ns": final["simulated_time_ns"]})
    assert results[0] == results[1], "Repeated seed and N produced different CPU states"
    (root / "xpbd-report.json").write_text(json.dumps(results[0], indent=2), encoding="utf-8")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--runner", required=True)
    parser.add_argument("--client", required=True)
    parser.add_argument("--work-dir", type=Path, required=True)
    parser.add_argument("--mode", choices=("stdio", "pipe"), required=True)
    args = parser.parse_args()
    root = args.work_dir / (args.mode + "-" + uuid4().hex)
    root.mkdir(parents=True)
    with host(args, root) as call:
        exercise(call, root)
    print(f"Simulation {args.mode}: idle ticks, pause, XPBD 300 steps and repeatability, Edit preservation and shutdown passed")


if __name__ == "__main__":
    main()
