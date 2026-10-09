"""M11.4 finite task acceptance: real SDK/IPC, GUI heartbeats and explicit safe boundaries."""
import argparse
from contextlib import contextmanager
from datetime import datetime
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import platform
import re
import subprocess
import sys
import time
from uuid import uuid4

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "sdk/python"))
from decker import Client, TransportError, guard

spec = importlib.util.spec_from_file_location("baseline", ROOT / "scripts/benchmark-simulation.py")
baseline = importlib.util.module_from_spec(spec)
spec.loader.exec_module(baseline)
save, distribution = baseline.save, baseline.distribution
PHASES = ("initializing", "first_batch", "steady")
ACTIONS = ("pause", "cancel", "stop", "shutdown")


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def read_case(directory, manifest, name):
    imported = manifest.get("imports", {}).get(name)
    path = Path(imported["path"]) if imported else directory / name / "control.json"
    if imported and digest(path) != imported["sha256"]:
        raise ValueError("Imported evidence changed: " + name)
    return json.loads(path.read_text(encoding="utf-8"))


def continuation(args, manifest):
    """Reuse every completed trial; only an explicitly identified manual interruption is excluded."""
    source = args.complete_from.resolve()
    original = json.loads((source / "manifest.json").read_text(encoding="utf-8"))
    for field in ("hosts", "backends", "sizes", "binaries", "gui_validation", "simulation_validation", "process_per_action"):
        if original.get(field) != manifest[field]:
            raise ValueError("Continuation configuration changed: " + field)
    if original.get("imports") or not args.interruption_reason:
        raise ValueError("Continuation requires an original collection and an explicit interruption reason")
    names = original["cases"]
    if len(set(names)) != len(names) or not names or args.interrupted_case != names[-1]:
        raise ValueError("Only the last manually interrupted trial may be excluded")
    plan, imports, counts, hot, phases = {}, {}, {}, {}, {}
    for name in names:
        path = source / name / "control.json"
        case = json.loads(path.read_text(encoding="utf-8"))
        if name == args.interrupted_case:
            if case["status"] != "failed" or original["status"] != "failed":
                raise ValueError("Excluded trial was not interrupted")
            continue
        if case["status"] != "passed":
            raise ValueError("Cannot omit or reuse a failed trial: " + name)
        imports[name] = {"path": str(path), "sha256": digest(path)}
        plan[name] = original["hot_queries"] if case["phase"] == "steady" else 0
        base = (case["host"], case["backend"], case["size"])
        counts[base] = counts.get(base, 0) + 1
        hot[base] = hot.get(base, 0) + sum(s.get("hot_sample", False) for s in case["samples"])
        phases.setdefault(base, {p: 0 for p in PHASES})[case["phase"]] += len(ACTIONS)
    for kind in args.hosts:
        for backend in args.backends:
            for size in args.sizes:
                base = kind, backend, size
                counts.setdefault(base, 0)
                hot.setdefault(base, 0)
                phases.setdefault(base, {p: 0 for p in PHASES})
                rep = original["repetitions"]
                while counts[base] < 20 or hot[base] < 100 or min(phases[base].values()) < 20:
                    phase = "steady" if hot[base] < 100 else min(PHASES, key=phases[base].get)
                    queries = max(0, 100 - hot[base]) if phase == "steady" else 0
                    plan[f"r{rep}-{kind}-{backend}-{size}-{phase}"] = queries
                    counts[base] += 1
                    phases[base][phase] += len(ACTIONS)
                    hot[base] += queries
                    rep += 1
    manifest.update(imports=imports, trial_plan=plan,
                    continuation={"source": str(source), "manifest_sha256": digest(source / "manifest.json"),
                                  "excluded_case": args.interrupted_case, "reason": args.interruption_reason,
                                  "excluded_sha256": digest(source / args.interrupted_case / "control.json")})
    return plan


def classify(run):
    task = run["task"]
    if not task["initialized"]:
        return "initializing"
    return "first_batch" if task["completed_steps"] == 0 else "steady"


@contextmanager
def host(args, kind, directory):
    directory.mkdir(parents=True)
    endpoint = "dk-response-" + uuid4().hex
    if kind == "editor":
        save(directory / "project.json", {"format": "DeckerProject", "version": 1, "name": "Response fixture",
             "scene": "scene.json", "assets": []})
        save(directory / "scene.json", {"format": "DeckerScene", "version": 1, "scene_id": str(uuid4()),
             "revision": 0, "entities": []})
        (directory / ".dk-editor-smoke").touch()
        command = [args.editor, "--root", directory, "--pipe", endpoint, "--fixture-camera",
                   "--validation" if args.gui_validation == "required" else "--no-validation",
                   "--simulation-response-probe"]
    else:
        command = [args.runner, "--project-root", directory, "--pipe", endpoint, "--pipe-timeout-ms", "60000"]
    with (directory / "stdout.log").open("wb") as out, (directory / "stderr.log").open("wb") as err:
        process = subprocess.Popen([str(x) for x in command], stdin=subprocess.DEVNULL, stdout=out, stderr=err,
                                   creationflags=subprocess.CREATE_NO_WINDOW)
        client = Client(endpoint, args.ctl, timeout=10)
        try:
            deadline = time.monotonic() + 30
            while True:
                try:
                    client.hello(timeout=.3)
                    break
                except TransportError:
                    if process.poll() is not None or time.monotonic() >= deadline:
                        raise
                    time.sleep(.01)
            if kind == "editor":
                while not (directory / "response-ready.json").exists():
                    if process.poll() is not None or time.monotonic() >= deadline:
                        raise RuntimeError("GUI never presented its ready frame")
                    time.sleep(.01)
            else:
                client.call("scene.new", {"name": "Response fixture"})
            yield client, process
            if process.poll() is None:
                client.call("runtime.shutdown")
            if process.wait(timeout=10):
                raise RuntimeError("Host shutdown failed")
        finally:
            if process.poll() is None:
                process.kill()
                process.wait(timeout=10)
    output = (directory / "stdout.log").read_text(encoding="utf-8")
    if kind == "editor":
        counts = re.search(r"validation errors=(\d+) warnings=(\d+) liveAllocations=(\d+) knownLoaderWarnings=(\d+)", output)
        if not counts or counts[1] != "0" or counts[3] != "0" or counts[2] != counts[4]:
            raise RuntimeError("GUI validation/lifetime check failed: " + output)
    elif output:
        raise RuntimeError("Runner emitted non-protocol stdout")
    diagnostics = (directory / "stderr.log").read_text(encoding="utf-8").splitlines()
    if any("VK_LAYER_AMD_switchable_graphics uses API version 1.3" not in line for line in diagnostics if line.strip()):
        raise RuntimeError("Unexpected diagnostics: " + "\n".join(diagnostics))


def query(client):
    value = client.call("simulation.query").value
    if value["run"]:
        run, task = value["run"], value["run"]["task"]
        if (run["fault"] is not None or task["status"] == "failed" or
                not 0 <= task["submitted_steps"] - task["completed_steps"] <= 8 or run["steps"] != task["completed_steps"]):
            raise RuntimeError("Invalid task progress: " + str(value))
    return value


def await_state(client, predicate):
    deadline = time.monotonic() + 10
    while True:
        value = query(client)
        if predicate(value):
            return value
        if time.monotonic() >= deadline:
            raise TimeoutError("Task boundary timed out: " + str(value))
        time.sleep(.002)


def timed(client, method, params, samples, **metadata):
    sample = {"method": method, "status": "failed", **metadata, "start_ns": time.perf_counter_ns()}
    samples.append(sample)  # Preserve a timeout/error sample, including when call raises.
    try:
        sample["value"] = client.call(method, params).value
        sample["status"] = "passed"
        return sample
    except Exception as error:
        sample["error"] = repr(error)
        raise
    finally:
        sample["latency_ns"] = time.perf_counter_ns() - sample["start_ns"]


def exercise(args, directory, kind, backend, size, phase, repetition):
    result = {"status": "failed", "host": kind, "backend": backend, "size": size, "phase": phase,
              "repetition": repetition, "samples": []}
    try:
        for action in args.actions:
            with host(args, kind, directory / action) as (client, process):
                before = client.call("scene.query").value
                history = client.call("history.status").value
                config = {"guard": guard(before["state"]), "count": 1000000, "solver": "xpbd_" + backend,
                          "batch_steps": 8, "fixed_dt_ns": 10000000,
                          "cloth": {"columns": size, "rows": size, "seed": 42, "iterations": 12}}
                common = {"action": action, "phase": phase, "process_first_task": True}
                accepted = timed(client, "simulation.run", config, result["samples"], **common)
                key = {"run_id": accepted["value"]["run"]["run_id"]}
                state = query(client)
                if phase == "first_batch":
                    state = await_state(client, lambda s: s["run"]["task"]["initialized"])
                elif phase == "steady":
                    state = await_state(client, lambda s: s["run"]["steps"] >= 32)
                actual_phase = classify(state["run"])
                common["actual_phase"] = actual_phase
                common["before_control"] = state["run"]["task"]
                # The short CPU stages can finish before an SDK request. Retain that evidence.
                if backend == "gpu" and actual_phase != phase:
                    raise RuntimeError(f"GPU {phase} was missed: {state}")
                observed = timed(client, "simulation.query", None, result["samples"], **common)
                common["observed_phase"] = classify(observed["value"]["run"])
                if phase == "steady" and action == "pause":
                    for _ in range(args.hot_queries):
                        timed(client, "simulation.query", None, result["samples"], **common, hot_sample=True)
                method = "runtime.shutdown" if action == "shutdown" else "simulation." + action
                control = timed(client, method, None if action == "shutdown" else key, result["samples"], **common)
                if action == "shutdown":
                    if process.wait(timeout=10):
                        raise RuntimeError("Shutdown exited with error")
                    control["boundary_ns"] = time.perf_counter_ns() - control["start_ns"]
                elif action == "stop":
                    final = await_state(client, lambda s: s["mode"] == "edit")
                    control["boundary_ns"] = time.perf_counter_ns() - control["start_ns"]
                else:
                    expected = "paused" if action == "pause" else "cancelled"
                    final = await_state(client, lambda s: s["run"]["task"]["status"] == expected)
                    control["boundary_ns"] = time.perf_counter_ns() - control["start_ns"]
                    if final["run"]["steps"] > control["value"]["run"]["task"]["submitted_steps"] + 8:
                        raise RuntimeError("Task advanced beyond the accepted control boundary")
                    time.sleep(.05)
                    if query(client) != final:
                        raise RuntimeError("Settled pause/cancel was not stable")
                if action != "shutdown":
                    control["final"] = final
                    if action != "stop":
                        client.call("simulation.stop", key)
                        await_state(client, lambda s: s["mode"] == "edit")
                    if client.call("scene.query").value != before or client.call("history.status").value != history:
                        raise RuntimeError("Simulation changed Edit/history")
            if kind == "editor":
                output = (directory / action / "stdout.log").read_text(encoding="utf-8")
                device = re.search(r"simulation_device=(shared|independent) queues=([12])", output)
                if not device or (device[1] == "shared" and device[2] != "2"):
                    raise RuntimeError("Missing or invalid simulation device provenance")
                result.setdefault("devices", {})[action] = {"mode": device[1], "queues": int(device[2]),
                    "validation": args.gui_validation if device[1] == "shared" else "if_available"}
                heartbeat = json.loads((directory / action / "response-heartbeat.json").read_text(encoding="utf-8"))
                intervals = result.setdefault("heartbeat_ns", {"initializing": [], "running": [], "control": []})
                for left, right in zip(heartbeat["samples"], heartbeat["samples"][1:]):
                    label = left[1]
                    group = "initializing" if label == "initializing" else "running" if label == "running" else "control"
                    if label not in ("edit", "paused", "succeeded", "cancelled", "failed"):
                        intervals[group].append(right[0] - left[0])
        result["status"] = "passed"
    except Exception as error:
        result["error"] = repr(error)
        raise
    finally:
        save(directory / "control.json", result)
    return result


def summarize(directory):
    # Invalidate a previous success before validating changed/incomplete raw inputs.
    save(directory / "summary.json", {"schema": 1, "status": "failed", "error": "Aggregation did not finish"})
    manifest = json.loads((directory / "manifest.json").read_text(encoding="utf-8"))
    if manifest["status"] != "passed":
        raise ValueError("Cannot summarize failed/incomplete collection")
    if not manifest.get("process_per_action"):
        raise ValueError("Cold controls require an independent process for every action")
    expected = set(manifest["trial_plan"]) if "trial_plan" in manifest else {f"r{rep}-{kind}-{backend}-{size}-{phase}"
                for rep in range(manifest["repetitions"]) for kind in manifest["hosts"]
                for backend in manifest["backends"] for size in manifest["sizes"] for phase in PHASES}
    if len(manifest["cases"]) != len(expected) or set(manifest["cases"]) != expected:
        raise ValueError("Missing or duplicate response trials")
    source = manifest.get("continuation")
    if source:
        root = Path(source["source"])
        if (digest(root / "manifest.json") != source["manifest_sha256"] or
                digest(root / source["excluded_case"] / "control.json") != source["excluded_sha256"]):
            raise ValueError("Interrupted source evidence changed")
    groups, pooled, hearts, coverage, trials = {}, {}, {}, {}, {}
    for name in manifest["cases"]:
        case = read_case(directory, manifest, name)
        identity = f"r{case['repetition']}-{case['host']}-{case['backend']}-{case['size']}-{case['phase']}"
        if identity != name:
            raise ValueError("Trial identity mismatch: " + name)
        if case["status"] != "passed":
            raise ValueError("Failed trial: " + name)
        for action in manifest.get("actions", ACTIONS):
            requests = [s for s in case["samples"] if s["action"] == action]
            method = "runtime.shutdown" if action == "shutdown" else "simulation." + action
            controls = [s for s in requests if s["method"] == method]
            if (len(controls) != 1 or "boundary_ns" not in controls[0]
                    or sum(s["method"] == "simulation.run" for s in requests) != 1
                    or not any(s["method"] == "simulation.query" for s in requests)):
                raise ValueError("Missing acceptance/query/control/boundary evidence: " + name)
            expected_hot = (manifest.get("trial_plan", {}).get(name, manifest["hot_queries"])
                            if action == "pause" and case["phase"] == "steady" else 0)
            if sum(s.get("hot_sample", False) for s in requests) != expected_hot:
                raise ValueError("Missing hot query evidence: " + name)
        if case["host"] == "editor" and not any(case.get("heartbeat_ns", {}).values()):
            raise ValueError("No active GUI event-loop heartbeat evidence: " + name)
        base = f"{case['host']}/{case['backend']}/{case['size']}"
        trials.setdefault(base, {p: 0 for p in PHASES})[case["phase"]] += len(manifest.get("actions", ACTIONS))
        for s in case["samples"]:
            if s["status"] != "passed":
                raise ValueError("Failed request: " + name)
            key = f"{base}/{case['phase']}/{s['method']}"
            groups.setdefault(key, []).append(s["latency_ns"])
            pooled.setdefault(f"{base}/{s['method']}", []).append(s["latency_ns"])
            if s.get("hot_sample"):
                coverage[base] = coverage.get(base, 0) + 1
            if "boundary_ns" in s:
                groups.setdefault(key + "/boundary", []).append(s["boundary_ns"])
                pooled.setdefault(f"{base}/{s['method']}/boundary", []).append(s["boundary_ns"])
        for phase, values in case.get("heartbeat_ns", {}).items():
            if values:
                hearts.setdefault(base + "/" + phase, []).extend(values)
    distributions = {key: distribution(values) for key, values in groups.items()}
    aggregate = {key: distribution(values) for key, values in pooled.items()}
    heartbeats = {key: distribution(values) for key, values in hearts.items()}
    bases = {f"{h}/{b}/{s}" for h in manifest["hosts"] for b in manifest["backends"] for s in manifest["sizes"]}
    if set(trials) != bases:
        raise ValueError("Missing host/backend/size coverage")
    complete = all(coverage.get(base, 0) >= 100 and min(trials[base].values()) >= 20 and
                   all(len(pooled.get(base + "/" + ("runtime.shutdown" if a == "shutdown" else "simulation." + a), [])) >= 20
                       for a in ACTIONS) for base in bases)
    failures = []
    for key, d in aggregate.items():
        # Pausing may await full initialization; the published target bounds its ACK, not that wait.
        if key.endswith("simulation.pause/boundary"):
            continue
        if key.endswith("runtime.shutdown/boundary"):
            p95, maximum = 400e6, 1000e6
        elif key.endswith("/boundary") or "runtime.shutdown" in key or "simulation.stop" in key:
            p95, maximum = 250e6, 1000e6
        else:
            p95, maximum = 100e6, 250e6
        if (d["n"] >= 20 and d["p95"] > p95) or d["max"] > maximum:
            failures.append({"key": key, "observed": d, "p95_limit_ns": p95, "max_limit_ns": maximum})
    for key, d in heartbeats.items():
        if (d["n"] >= 20 and d["p95"] > 50e6) or d["max"] > 100e6:
            failures.append({"key": "heartbeat/" + key, "observed": d, "p95_limit_ns": 50e6, "max_limit_ns": 100e6})
    if complete and any(n < 100 for n in coverage.values()):
        raise ValueError("Insufficient hot query samples")
    summary = {"schema": 1, "status": "failed" if failures else "passed" if complete else "partial",
               "groups": distributions, "aggregate": aggregate, "heartbeat": heartbeats,
               "phase_trial_count": trials, "hot_query_count": coverage, "failures": failures,
               "exit_limits_ns": {"p95": 400000000, "max": 1000000000}}
    save(directory / "summary.json", summary)
    if failures:
        raise ValueError(f"{len(failures)} response thresholds failed; see {directory / 'summary.json'}")
    return summary


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--runner", type=Path, default=ROOT / "out/build/windows-graphics/bin/RelWithDebInfo/dk-run.exe")
    parser.add_argument("--editor", type=Path, default=ROOT / "out/build/windows-editor/bin/RelWithDebInfo/dk-editor.exe")
    parser.add_argument("--ctl", type=Path, default=ROOT / "out/build/windows-graphics/bin/RelWithDebInfo/dk-ctl.exe")
    parser.add_argument("--hosts", nargs="+", choices=("runner", "editor"), default=["runner", "editor"])
    parser.add_argument("--backends", nargs="+", choices=("cpu", "gpu"), default=["cpu", "gpu"])
    parser.add_argument("--sizes", nargs="+", type=int, choices=(8, 16, 32), default=[8, 16, 32])
    parser.add_argument("--actions", nargs="+", choices=ACTIONS, default=list(ACTIONS))
    parser.add_argument("--repetitions", type=int, default=7)
    parser.add_argument("--hot-queries", type=int, default=15)
    parser.add_argument("--smoke", action="store_true")
    parser.add_argument("--gui-validation", choices=("required", "disabled"), default="required")
    parser.add_argument("--output", type=Path)
    parser.add_argument("--summarize", type=Path)
    parser.add_argument("--complete-from", type=Path)
    parser.add_argument("--interrupted-case")
    parser.add_argument("--interruption-reason")
    args = parser.parse_args()
    if args.complete_from and args.actions != list(ACTIONS):
        parser.error("Continuation requires all original control actions")
    if args.summarize:
        summarize(args.summarize.resolve())
        return
    if args.smoke:
        args.repetitions = 1
    elif not args.complete_from and (not 7 <= args.repetitions <= 40 or
            ("pause" in args.actions and args.hot_queries * args.repetitions < 100)):
        parser.error("Formal runs require 7..40 phase repetitions (at least 20 controls total) and 100 hot queries")
    directory = args.output or ROOT / "out/benchmarks" / ("response-" + datetime.now().strftime("%Y%m%d-%H%M%S") + "-" + uuid4().hex[:8])
    directory = directory.resolve()
    directory.mkdir(parents=True, exist_ok=False)
    binaries = [args.ctl] + ([args.runner] if "runner" in args.hosts else []) + ([args.editor] if "editor" in args.hosts else [])
    manifest = {"schema": 1, "status": "failed", "started_at": datetime.now().astimezone().isoformat(),
        "repetitions": args.repetitions, "hot_queries": args.hot_queries, "hosts": args.hosts, "sizes": args.sizes,
        "backends": args.backends, "platform": platform.platform(), "python": platform.python_version(),
        "actions": args.actions,
        "gui_validation": args.gui_validation, "simulation_validation": "independent_if_available_or_shared_gui",
        "simulation_device_policy": "editor_secondary_queue_when_available",
        "process_per_action": True,
        "sources": {str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest() for p in (
            Path(__file__), ROOT / "engine/editor/src/ResponseProbe.hpp",
            ROOT / "tests/integration/SimulationResponseTest.py")},
        "commit": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip(),
        "binaries": {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in binaries}, "cases": []}
    for kind, binary in (("runner", args.runner), ("editor", args.editor)):
        if kind in args.hosts:
            cache = binary.parents[2] / "CMakeCache.txt"
            (directory / (kind + "-CMakeCache.txt")).write_bytes(cache.read_bytes())
    (directory / "source-diff.patch").write_bytes(subprocess.check_output(["git", "diff", "HEAD"], cwd=ROOT))
    (directory / "power.txt").write_bytes(subprocess.check_output(["powercfg", "/getactivescheme"]))
    print(directory, flush=True)
    try:
        plan = continuation(args, manifest) if args.complete_from else {
            f"r{rep}-{kind}-{backend}-{size}-{phase}": args.hot_queries if phase == "steady" else 0
            for rep in range(args.repetitions) for kind in args.hosts
            for size in args.sizes[rep % len(args.sizes):] + args.sizes[:rep % len(args.sizes)]
            for backend in args.backends for phase in PHASES}
        manifest["trial_plan"] = plan
        for name, queries in plan.items():
            manifest["cases"].append(name)
            if name not in manifest.get("imports", {}):
                rep, kind, backend, size, phase = name.split("-")
                print(name, flush=True)
                args.hot_queries = queries
                exercise(args, directory / name, kind, backend, int(size), phase, int(rep[1:]))
            save(directory / "manifest.json", manifest)
        if any(hashlib.sha256(p.read_bytes()).hexdigest() != manifest["binaries"][str(p)] for p in binaries):
            raise RuntimeError("Measured binaries changed during collection")
        manifest["status"] = "passed"
    except Exception as error:
        manifest["error"] = repr(error)
        raise
    finally:
        manifest["finished_at"] = datetime.now().astimezone().isoformat()
        save(directory / "manifest.json", manifest)
    summarize(directory)
    print("Response measurements complete: " + str(directory), flush=True)


if __name__ == "__main__":
    main()
