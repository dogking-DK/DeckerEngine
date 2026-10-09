"""Real Named Pipe latency fixture; invoked by benchmark-simulation.py, no SDK changes."""
from contextlib import contextmanager
import json
import os
from pathlib import Path
import subprocess
import struct
import sys
import time
from uuid import uuid4

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "sdk/python"))
from decker import Client, TransportError, guard


def timed(client, method, params=None):
    start = time.perf_counter_ns()
    reply = client.call(method, params).value
    end = time.perf_counter_ns()
    return {"method": method, "start_ns": start, "end_ns": end, "latency_ns": end - start, "value": reply}


def busy_process(endpoint, request_path, ready_path, result_path):
    """Send one framed request before signaling readiness; parent enforces the lifetime bound."""
    request = json.loads(Path(request_path).read_text(encoding="utf-8"))
    with open(r"\\.\pipe\DeckerEngine." + endpoint, "r+b", buffering=0) as pipe:
        def send(value):
            data = json.dumps(value).encode("utf-8")
            view = memoryview(struct.pack("<I", len(data)) + data)
            while view:
                count = pipe.write(view)
                if not count:
                    raise RuntimeError("busy pipe write failed")
                view = view[count:]

        def receive():
            def exact(count):
                data = bytearray()
                while len(data) < count:
                    chunk = pipe.read(count - len(data))
                    if not chunk:
                        raise RuntimeError("busy pipe disconnected")
                    data.extend(chunk)
                return data
            count = struct.unpack("<I", exact(4))[0]
            if not 0 < count <= 4 * 1024 * 1024:
                raise RuntimeError("invalid busy response size")
            return json.loads(exact(count))

        send({"protocol": 1, "hello": True, "reserve": True})
        hello = receive()
        start = time.perf_counter_ns()
        send({"protocol": 1, "session": hello["session"], "request": {
            "jsonrpc": "2.0", "id": hello["request_id"], **request}})
        Path(ready_path).write_text(str(time.perf_counter_ns()), encoding="utf-8")
        response = receive()["response"]
        end = time.perf_counter_ns()
        if response["id"] != hello["request_id"] or "error" in response:
            raise RuntimeError(str(response))
        result = {"method": request["method"], "start_ns": start, "end_ns": end,
                  "latency_ns": end - start, "value": response["result"]["value"], "scope": "preconnected raw pipe request"}
    Path(result_path).write_text(json.dumps(result), encoding="utf-8")


def contend(client, directory, busy_method, busy_params, control_method, control_params=None):
    # A ready file is emitted only AFTER the busy request is written on an established pipe.
    # This prevents process-start scheduling from letting shutdown overtake the intended work.
    prefix = directory / ("busy-" + uuid4().hex)
    request, ready, result = (prefix.with_suffix(suffix) for suffix in (".request.json", ".ready", ".result.json"))
    request.write_text(json.dumps({"method": busy_method, "params": busy_params}), encoding="utf-8")
    with prefix.with_suffix(".stderr.log").open("wb") as error:
        worker = subprocess.Popen([sys.executable, __file__, "--busy", client.endpoint, str(request), str(ready), str(result)],
                                  stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=error, creationflags=subprocess.CREATE_NO_WINDOW)
        try:
            deadline = time.monotonic() + 15
            while not ready.exists():
                if worker.poll() is not None or time.monotonic() >= deadline:
                    raise RuntimeError("busy request was not sent")
                time.sleep(0.001)
            time.sleep(0.05)
            control = timed(client, control_method, control_params)
            if worker.wait(timeout=60):
                raise RuntimeError("busy request failed")
            busy = json.loads(result.read_text(encoding="utf-8"))
        finally:
            if worker.poll() is None:
                worker.kill()
                worker.wait(timeout=10)
    control["busy"] = busy
    control["client_intervals_overlap"] = control["start_ns"] < busy["end_ns"]
    control["send_delay_ns"] = control["start_ns"] - busy["start_ns"]
    return control


@contextmanager
def host(runner, ctl, directory):
    directory.mkdir(parents=True)
    endpoint = "dk-bench-" + uuid4().hex
    with (directory / "stderr.log").open("wb") as err, (directory / "stdout.log").open("wb") as out:
        process = subprocess.Popen([str(runner), "--project-root", str(directory), "--pipe", endpoint,
                                    "--pipe-timeout-ms", "60000"], stdin=subprocess.DEVNULL, stdout=out, stderr=err,
                                   creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0)
        client = Client(endpoint, ctl, timeout=60)
        try:
            deadline = time.monotonic() + 15
            while True:
                try:
                    client.hello(timeout=0.3)
                    break
                except TransportError:
                    if process.poll() is not None or time.monotonic() >= deadline:
                        raise
                    time.sleep(0.02)
            yield client, process
            if process.poll() is None:
                client.call("runtime.shutdown")
            if process.wait(timeout=60) != 0:
                raise RuntimeError("runner failed")
        finally:
            if process.poll() is None:
                process.kill()
                process.wait(timeout=10)
    if (directory / "stdout.log").stat().st_size:
        raise RuntimeError("pipe runner emitted stdout")
    lines = (directory / "stderr.log").read_text(encoding="utf-8").splitlines()
    unexpected = [line for line in lines if line.strip() and
                  "VK_LAYER_AMD_switchable_graphics uses API version 1.3" not in line]
    if unexpected:
        raise RuntimeError("runner diagnostics: " + "\n".join(unexpected))


def measure(runner, ctl, directory, size, backend):
    result = {"schema": 1, "status": "failed", "size": size, "backend": backend,
              "scope": "Python SDK + dk-ctl startup/hello + pipe + owner + parse; service validation if_available",
              "samples": [], "cancel": "unsupported", "delay_ms": 50}
    samples = result["samples"]

    def record(sample, phase):
        sample["phase"] = phase
        samples.append(sample)
        return sample["value"]

    try:
        # Separate sessions retain genuinely first-use step/initialization for each control.
        for action in ("query", "pause", "stop", "shutdown"):
            with host(runner, ctl, directory / action) as (client, process):
                state = client.call("scene.new", {"name": "Latency baseline"}).value
                before = client.call("scene.query").value
                if action == "query":
                    names = {c["name"] for c in client.call("commands.list").value}
                    if "simulation.cancel" in names:
                        raise RuntimeError("cancel contract changed: extend this baseline fixture")
                    for _ in range(5):
                        record(timed(client, "simulation.query"), "idle")
                params = {"guard": guard(state), "solver": "xpbd_" + backend, "paused": True,
                          "fixed_dt_ns": 10000000, "max_catch_up_steps": 8,
                          "cloth": {"columns": size, "rows": size, "seed": 42, "spacing": 0.15,
                                    "height": 0.75, "particle_mass": 0.1, "compliance": 1e-6,
                                    "iterations": 12, "gravity_y": -9.81, "floor_y": 0, "damping": 0.5}}
                if action == "query":
                    sample = contend(client, directory / action, "simulation.start", params, "simulation.query")
                    started = sample["busy"]["value"]
                    record(sample, "cold_start")
                else:
                    started = client.call("simulation.start", params).value
                identity = {"run_id": started["run"]["run_id"]}
                method = "runtime.shutdown" if action == "shutdown" else "simulation." + action
                control_params = identity if action in ("pause", "stop") else None
                sample = contend(client, directory / action, "simulation.step", {**identity, "count": 8}, method, control_params)
                value = record(sample, "cold_step")
                if sample["busy"]["value"]["run"]["steps"] != 8:
                    raise RuntimeError("cold step did not commit exactly 8 steps")
                if action in ("query", "pause") and value["run"]["steps"] != 8:
                    raise RuntimeError("control was served before the busy step; contention sample invalid")
                if action == "shutdown":
                    process.wait(timeout=60)
                    sample["exit_since_request_ns"] = time.perf_counter_ns() - sample["start_ns"]
                    continue
                if action == "stop":
                    if client.call("scene.query").value != before or value["mode"] != "edit":
                        raise RuntimeError("cold Stop changed Edit")
                    continue
                if action == "pause":
                    first = client.call("simulation.query").value
                    time.sleep(0.05)
                    if client.call("simulation.query").value != first:
                        raise RuntimeError("cold pause was not stable")
                    continue
                # The query session also measures the normal running owner pump after warmup.
                for _ in range(3):
                    client.call("simulation.step", {**identity, "count": 8})
                client.call("simulation.resume", identity)
                time.sleep(0.1)
                hot_steps = []
                for _ in range(5):
                    running = record(timed(client, "simulation.query"), "hot_running")
                    if running["mode"] != "running":
                        raise RuntimeError("hot query not running")
                    hot_steps.append(running["run"]["steps"])
                    time.sleep(0.02)
                if hot_steps[-1] <= hot_steps[0]:
                    raise RuntimeError("owner did not advance during hot queries")
                paused = record(timed(client, "simulation.pause", identity), "hot_running")
                time.sleep(0.05)
                if client.call("simulation.query").value != paused or paused["mode"] != "paused":
                    raise RuntimeError("hot pause was not stable")
                client.call("simulation.resume", identity)
                time.sleep(0.1)
                stopped = record(timed(client, "simulation.stop", identity), "hot_running")
                if stopped["mode"] != "edit" or client.call("scene.query").value != before:
                    raise RuntimeError("hot Stop changed Edit")
                # A second run is warmed before exit; process is still the same owner.
                started = client.call("simulation.start", params).value
                identity = {"run_id": started["run"]["run_id"]}
                for _ in range(4):
                    client.call("simulation.step", {**identity, "count": 8})
                client.call("simulation.resume", identity)
                time.sleep(0.1)
                sample = timed(client, "runtime.shutdown")
                process.wait(timeout=60)
                sample["exit_since_request_ns"] = time.perf_counter_ns() - sample["start_ns"]
                record(sample, "hot_running")
        result["status"] = "passed"
    except Exception as error:
        result["error"] = repr(error)
        raise
    finally:
        directory.mkdir(parents=True, exist_ok=True)
        (directory / "control.json").write_text(json.dumps(result, indent=2, allow_nan=False) + "\n", encoding="utf-8")
    return result


if __name__ == "__main__":
    if len(sys.argv) != 6 or sys.argv[1] != "--busy":
        raise SystemExit("Internal fixture: use benchmark-simulation.py")
    busy_process(*sys.argv[2:])
