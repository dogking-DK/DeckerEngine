"""Finite task lifecycle through a real pipe host; retain timings and numeric evidence."""
import argparse
import json
from pathlib import Path
import time
from uuid import uuid4

from decker import RpcError, guard
from PythonClientTest import server


def rejected(client, method, params):
    try:
        client.call(method, params)
    except RpcError:
        return
    raise AssertionError(f"Unexpected success: {method}")


def wait(client, predicate, timeout=30):
    deadline = time.monotonic() + timeout
    while True:
        state = client.call("simulation.query").value
        if state["run"]:
            task = state["run"]["task"]
            assert state["run"]["fault"] is None, state
            assert 0 <= task["submitted_steps"] - task["completed_steps"] <= task["batch_steps"], state
            assert state["run"]["steps"] == task["completed_steps"], state
        if predicate(state):
            return state
        assert time.monotonic() < deadline, state
        time.sleep(.005)


def settled(client, status):
    return wait(client, lambda s: s["run"]["task"]["status"] == status)["run"]


def stop(client, key):
    state = client.call("simulation.stop", key).value
    assert state["mode"] in ("stopping", "edit"), state
    wait(client, lambda s: s["mode"] == "edit")


def particles(client, key, size):
    result = []
    for offset in range(0, size * size, 256):
        page = client.call("simulation.particles", {**key, "offset": offset, "limit": 256}).value
        assert page["steps"] == 300
        result.extend(page["particles"])
    assert len(result) == size * size
    return result


def exercise(args, root, report):
    with server(args, root, "dk-task-" + uuid4().hex) as client:
        client.timeout = 60
        state = client.call("scene.new", {"name": "Finite task EditWorld"}).value
        before = client.call("scene.query").value
        history = client.call("history.status").value
        caps = client.call("runtime.capabilities").value
        assert caps["simulation"]["finite_tasks"] and not caps["async_tasks"]
        assert caps["simulation"]["max_in_flight_batches"] == 1
        config = {"guard": guard(state), "solver": "xpbd_gpu" if args.gpu else "xpbd_cpu",
                  "count": 1000000, "batch_steps": 8, "fixed_dt_ns": 10000000,
                  "cloth": {"columns": 32, "rows": 32, "seed": 42}}

        def timed(method, params=None):
            start = time.perf_counter()
            result = client.call(method, params).value
            report["timings_ms"].setdefault(method, []).append((time.perf_counter() - start) * 1000)
            return result

        run = timed("simulation.run", config)["run"]
        key = {"run_id": run["run_id"]}
        # Cold initialization and compilation proceed independently of metadata queries/control.
        timed("runtime.capabilities")
        timed("simulation.query")
        paused = timed("simulation.pause", key)["run"]
        bound = paused["task"]["submitted_steps"] + 8
        run = settled(client, "paused")
        assert run["steps"] <= bound
        time.sleep(.08)
        assert client.call("simulation.query").value["run"] == run
        rejected(client, "simulation.step", {**key, "count": 1})
        timed("simulation.resume", key)
        wait(client, lambda s: s["run"]["steps"] > run["steps"])
        rejected(client, "simulation.particles", key)
        if args.gpu:
            rejected(client, "simulation.export", {**key, "expected_steps": 0, "output": "busy"})
        rejected(client, "simulation.cancel", {"run_id": str(uuid4())})
        cancelled = timed("simulation.cancel", key)["run"]
        run = settled(client, "cancelled")
        assert run["steps"] <= cancelled["task"]["submitted_steps"] + 8
        assert run["task"]["submitted_steps"] == run["steps"]
        assert client.call("simulation.cancel", key).value["run"] == run
        rejected(client, "simulation.resume", key)
        stop(client, key)

        for size in (8, 16, 32):
            cloth = {"columns": size, "rows": size, "seed": 42}
            # Existing synchronous CPU path supplies an independent scheduling reference.
            reference = client.call("simulation.start", {"guard": guard(state), "solver": "xpbd_cpu",
                "paused": True, "fixed_dt_ns": 10000000, "cloth": cloth}).value["run"]
            reference_key = {"run_id": reference["run_id"]}
            for first in range(0, 300, 8):
                client.call("simulation.step", {**reference_key, "count": min(8, 300 - first)})
            expected = particles(client, reference_key, size)
            client.call("simulation.stop", reference_key)
            run = timed("simulation.run", {**config, "count": 300, "cloth": cloth})["run"]
            key = {"run_id": run["run_id"]}
            run = settled(client, "succeeded")
            assert run["steps"] == 300 and run["simulated_time_ns"] == 3000000000
            assert run["task"]["submitted_steps"] == 300 and run["metrics"] is None
            actual = particles(client, key, size)
            errors = {field: max(abs(a-b) for p, q in zip(actual, expected)
                                for a, b in zip(p[field], q[field])) for field in ("position", "velocity")}
            assert errors["position"] < .002 and errors["velocity"] < .02, errors
            if not args.gpu:
                assert actual == expected
            assert actual[:size] == expected[:size], "Pinned particles moved"
            assert min(p["position"][1] for p in actual) >= -1e-6, "Particles penetrated the floor"
            report["results"].append({"size": size, "run": run, "errors": errors})
            if args.gpu and size == 8:
                exported = client.call("simulation.export", {**key, "expected_steps": 300, "output": "finite"}).value
                assert len(exported["files"]) == 5
                assert (root / "finite/image.ppm").is_file()
                assert particles(client, key, size) == actual, "Export advanced or changed simulation"
            stop(client, key)
            assert client.call("scene.query").value == before
            assert client.call("history.status").value == history
        # Leave a genuinely active task for the fixture's runtime.shutdown + process wait.
        timed("simulation.run", config)
        wait(client, lambda s: s["run"]["steps"] > 0)


def cold_close(args, root, mode):
    with server(args, root, "dk-task-cold-" + uuid4().hex) as client:
        state = client.call("scene.new").value
        run = client.call("simulation.run", {"guard": guard(state), "count": 1000000,
            "solver": "xpbd_gpu" if args.gpu else "xpbd_cpu"}).value["run"]
        key = {"run_id": run["run_id"]}
        if mode == "stop":
            stop(client, key)
        elif mode == "cancel":
            accepted = client.call("simulation.cancel", key).value["run"]
            final = settled(client, "cancelled")
            assert final["steps"] <= accepted["task"]["submitted_steps"] + 8
        # All cases exit via runtime.shutdown, including a task still initializing.


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--runner", required=True)
    parser.add_argument("--client", required=True)
    parser.add_argument("--work-dir", type=Path, required=True)
    parser.add_argument("--gpu", action="store_true")
    args = parser.parse_args()
    args.pipe_timeout_ms = 60000
    root = args.work_dir / (("gpu-" if args.gpu else "cpu-") + uuid4().hex)
    root.mkdir(parents=True)
    report = {"gpu": args.gpu, "timings_ms": {}, "results": []}
    exercise(args, root, report)
    for mode in ("stop", "cancel", "shutdown"):
        case = root / mode
        case.mkdir()
        cold_close(args, case, mode)
    (root / "report.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
    print(f"Finite tasks: cold controls, exact 300 steps at three sizes, pause/cancel/stop/shutdown passed: {root}")


if __name__ == "__main__":
    main()
