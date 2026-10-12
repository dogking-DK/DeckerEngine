"""Serial finite-task experiment execution and durable progress on an existing pipe host."""

import argparse
from contextlib import contextmanager
import json
import os
from pathlib import Path
import sys
import tempfile
import threading
import time
from uuid import uuid4

from ._experiment_lock import named_mutex
from .client import Client
from .models import RpcError, TransportError, guard
from .experiments import (ExperimentError, PLAN_LIMIT, INDEX_LIMIT, canonical, clone_json, decode_json, expand_plan,
                          integer, keys, normalize_config, read_json, require, topology)
from .experiment_index import (_errors, _inside, _inspect, _now, _publish, _read_file, _relative_parts,
                               _root, _timestamp, _uuid, create_index, environment, register_artifacts, verify_index)

_leases = set()
_lease_guard = threading.Lock()
STATES = {"pending", "submitting", "running", "collecting", "stopping", "succeeded", "failed", "cancelled", "unknown"}
ENGINE_STATES = {"initializing", "running", "pausing", "paused", "cancelling", "cancelled", "succeeded", "failed", "stopping"}


class ExperimentBatchError(ExperimentError):
    """A batch stopped; progress and accepted runs are retained, never implicitly retried."""
    def __init__(self, cause, directory, progress, recording_error=None):
        super().__init__(f"Experiment batch stopped: {cause}" +
                         (f"; progress write also failed: {recording_error}" if recording_error else ""))
        self.cause, self.directory = cause, Path(directory)
        self.progress = clone_json(progress)
        self.recording_error = recording_error


class _Unconfirmed(ExperimentError):
    pass


class _Cancelled(ExperimentError):
    pass


@contextmanager
def _runtime_lease(session):
    """Named mutex serializes cooperating processes; a local set also rejects recursion."""
    _uuid(session)
    require(os.name == "nt", "Batch execution requires the Windows local pipe client")
    with _lease_guard:
        require(session not in _leases, "This Runtime already has a batch executor")
        _leases.add(session)
    try:
        with named_mutex(session, 0, "This Runtime already has a batch executor or its lease is unavailable"):
            yield
    finally:
        with _lease_guard:
            _leases.remove(session)


def _summary(report):
    counts = {name: sum(e["status"] == name for e in report["experiments"]) for name in sorted(STATES)}
    return {"batch_id": report["batch_id"], "plan_id": report["plan_id"], "status": report["status"],
            "sequence": report["sequence"], "counts": counts,
            "completed_steps": sum(e["completed_steps"] for e in report["experiments"]),
            "target_steps": sum(e["target_steps"] for e in report["experiments"])}


def read_progress(directory):
    """Read an atomic progress snapshot and verify every referenced successful artifact."""
    with _errors():
        root = _root(directory)
        report = decode_json(_read_file(_inside(root, "batch.json"), INDEX_LIMIT))
        keys(report, ("format", "version", "batch_id", "plan_id", "session", "endpoint", "source",
                      "created_at", "updated_at", "sequence", "status", "experiments", "error"))
        require(report["format"] == "DeckerExperimentBatch" and type(report["version"]) is int
                and report["version"] == 1, "Expected DeckerExperimentBatch v1")
        _uuid(report["batch_id"])
        _uuid(report["session"])
        _timestamp(report["created_at"])
        _timestamp(report["updated_at"])
        keys(report["source"], ("document_id", "scene_id", "revision", "dirty", "entity_count"))
        for field in ("document_id", "scene_id"):
            _uuid(report["source"][field])
        integer(report["source"]["revision"], 0, 2**64-1)
        integer(report["source"]["entity_count"], 0, 2**64-1)
        require(type(report["source"]["dirty"]) is bool, "Invalid source dirty flag")
        integer(report["sequence"], 0, 2**63-1)
        require(report["status"] in ("running", "succeeded", "failed", "cancelled", "unknown"), "Invalid batch status")
        index = verify_index(root)
        require(report["plan_id"] == index["plan_id"] and type(report["experiments"]) is list
                and len(report["experiments"]) == len(index["experiments"]), "Batch plan mismatch")
        active, unfinished_seen = 0, False
        for item, entry in zip(report["experiments"], index["experiments"]):
            keys(item, ("experiment_id", "directory", "target_steps", "submitted_steps", "completed_steps",
                        "batch_active", "initialized", "status", "task_status", "run_id", "submission", "last_call", "artifact_attempt_id", "error"))
            require(item["experiment_id"] == entry["experiment_id"] and item["directory"] == "results/" + entry["experiment_id"],
                    "Batch experiment identity mismatch")
            require(type(item["target_steps"]) is int and item["target_steps"] == entry["config"]["steps"], "Batch target mismatch")
            integer(item["submitted_steps"], 0, item["target_steps"])
            integer(item["completed_steps"], 0, item["submitted_steps"])
            require(type(item["batch_active"]) is bool and type(item["initialized"]) is bool, "Invalid task lifecycle flags")
            require(item["status"] in STATES and (item["task_status"] is None or item["task_status"] in ENGINE_STATES), "Invalid progress status")
            if item["run_id"] is not None:
                _uuid(item["run_id"])
                require(type(item["submission"]) is dict, "Accepted run has no submission receipt")
            if item["submission"] is not None:
                submission = item["submission"]
                keys(submission, ("method", "params", "execution", "ticket", "task_id"))
                require(submission["method"] == "simulation.run" and submission["execution"] == "received",
                        "Invalid finite task submission receipt")
                _uuid(submission["task_id"])
                keys(submission["ticket"], ("session", "request_id"))
                require(submission["ticket"]["session"] == report["session"], "Submission belongs to another Runtime")
                integer(submission["ticket"]["request_id"], 1, 2**63-1)
                config = entry["config"]
                expected = {"guard": guard(report["source"]), "count": max(1, config["steps"]),
                    "paused": config["steps"] == 0, "solver": config["solver"], "cloth": config["cloth"],
                    "fixed_dt_ns": config["fixed_dt_ns"], "batch_steps": 8}
                require(canonical(submission["params"]) == canonical(expected), "Submission differs from plan")
            if item["status"] != "succeeded":
                require(not unfinished_seen or item["status"] == "pending", "Batch has multiple unfinished attempts")
                unfinished_seen = True
            else:
                require(not unfinished_seen, "Batch succeeded out of serial order")
            if item["status"] == "pending":
                require(item["run_id"] is None and item["submission"] is None and item["artifact_attempt_id"] is None
                        and item["submitted_steps"] == 0 and item["completed_steps"] == 0, "Pending experiment contains execution evidence")
            if item["status"] in ("submitting", "running", "collecting", "stopping"):
                active += 1
            if item["artifact_attempt_id"] is not None:
                matches = [a for a in entry["attempts"] if a["attempt_id"] == item["artifact_attempt_id"]]
                require(len(matches) == 1 and matches[0]["source"]["run_id"] == item["run_id"]
                        and matches[0]["directory"] == item["directory"], "Batch artifact attempt mismatch")
                require(all(matches[0]["source"][k] == report["source"][k] for k in ("document_id", "scene_id", "revision")),
                        "Batch artifact scene source mismatch")
            if item["status"] == "succeeded":
                require(item["artifact_attempt_id"] is not None and item["completed_steps"] == item["target_steps"],
                        "Successful experiment lacks complete evidence")
        require(active <= 1, "Batch contains overlapping experiments")
        if report["status"] == "succeeded":
            require(all(e["status"] == "succeeded" for e in report["experiments"]), "Batch success is incomplete")
        return report


class _Runner:
    def __init__(self, client, index, root, output, source, session, observed, timeout, poll_interval, progress):
        self.client, self.index, self.root, self.output = client, index, root, output
        self.observed, self.timeout, self.poll_interval, self.progress = observed, timeout, poll_interval, progress
        self.current = None
        self.deadline = None
        self.report = {"format": "DeckerExperimentBatch", "version": 1, "batch_id": str(uuid4()),
            "plan_id": index["plan_id"], "session": session, "endpoint": client.endpoint, "source": source,
            "created_at": _now(), "updated_at": _now(), "sequence": 0, "status": "running", "error": None,
            "experiments": [{"experiment_id": e["experiment_id"], "directory": "results/" + e["experiment_id"],
                "target_steps": e["config"]["steps"], "submitted_steps": 0, "completed_steps": 0, "status": "pending",
                "batch_active": False, "initialized": False, "task_status": None, "run_id": None, "submission": None, "last_call": None,
                "artifact_attempt_id": None, "error": None} for e in index["experiments"]]}

    def save(self, *, notify=True):
        self.report["sequence"] += 1
        self.report["updated_at"] = _now()
        _publish(self.root, self.report, "batch.json")
        if notify and self.progress is not None:
            self.progress(clone_json({**_summary(self.report), "current": self.current}))

    def budget(self):
        left = self.deadline - time.monotonic()
        if left < .001:
            raise _Unconfirmed("Experiment deadline exceeded; accepted work was not cancelled")
        return min(self.client.timeout, left)

    def call(self, method, params=None):
        record = {"method": method, "params": params or {}, "execution": "not_sent", "ticket": None, "task_id": None}
        self.current["last_call"] = record
        timeout = self.budget()
        if method in ("simulation.run", "simulation.export", "simulation.stop"):
            record["execution"] = "unknown"  # intent durable before any externally visible action
            self.save()
            timeout = self.budget()
        try:
            reply = self.client.call(method, params, timeout=timeout)
        except (TransportError, RpcError) as error:
            record["execution"] = error.execution
            if error.ticket:
                record["ticket"] = {"session": error.ticket.session, "request_id": error.ticket.request_id}
            record["task_id"] = getattr(error, "task_id", None)
            raise
        record.update(execution="received", ticket={"session": reply.ticket.session, "request_id": reply.ticket.request_id},
                      task_id=reply.task_id)
        if reply.ticket.session != self.report["session"]:
            raise _Unconfirmed("Runtime session changed; response does not belong to this batch")
        return reply.value

    def observe(self, state, entry):
        require(type(state) is dict and type(state.get("run")) is dict, "Expected the accepted simulation run")
        run = state["run"]
        require(run["run_id"] == self.current["run_id"], "Simulation run changed during experiment")
        config = entry["config"]
        require(canonical(run["source"]) == canonical(self.report["source"]), "Simulation source changed")
        actual = {**config, "cloth": run["cloth"], "solver": run["solver"], "fixed_dt_ns": run["fixed_dt_ns"]}
        require(normalize_config(actual) == config, "Accepted simulation configuration differs from plan")
        task = run["task"]
        require(type(task) is dict and task["status"] in ENGINE_STATES, "Missing finite task status")
        require(type(task["target_steps"]) is int and task["target_steps"] == max(1, config["steps"]), "Finite task target mismatch")
        completed = integer(run["steps"], self.current["completed_steps"], config["steps"])
        submitted = integer(task["submitted_steps"], completed, config["steps"])
        require(type(task["completed_steps"]) is int and task["completed_steps"] == completed
                and type(task["in_flight_steps"]) is int and task["in_flight_steps"] == submitted-completed
                and submitted-completed <= integer(task["batch_steps"], 1, 8), "Finite task progress mismatch")
        require(type(run["simulated_time_ns"]) is int and run["simulated_time_ns"] == completed*config["fixed_dt_ns"], "Simulation time mismatch")
        require(type(task["initialized"]) is bool and type(task["batch_active"]) is bool, "Invalid task completion flags")
        changed = any(self.current[k] != v for k, v in {"completed_steps": completed, "submitted_steps": submitted,
            "task_status": task["status"], "batch_active": task["batch_active"], "initialized": task["initialized"]}.items())
        self.current.update(completed_steps=completed, submitted_steps=submitted, task_status=task["status"],
                            batch_active=task["batch_active"], initialized=task["initialized"])
        if changed:
            self.save()
        if task["status"] == "cancelled":
            raise _Cancelled("Runtime experiment was cancelled")
        require(run["fault"] is None and task["status"] != "failed", f"Runtime experiment failed: {run['fault']}")
        return run

    def settled(self, state, entry):
        while True:
            run = self.observe(state, entry)
            task = run["task"]
            desired = "succeeded" if entry["config"]["steps"] else "paused"
            if task["status"] == desired and task["initialized"] and not task["batch_active"]:
                require(state["mode"] == "paused" and run["steps"] == entry["config"]["steps"]
                        and task["submitted_steps"] == run["steps"], "Experiment has no stable completion boundary")
                return run
            time.sleep(min(self.poll_interval, self.budget()))
            state = self.call("simulation.query")

    def numeric(self, entry, run):
        identity = {"run_id": run["run_id"]}
        config = entry["config"]
        state = self.call("simulation.sample", {**identity, "expected_steps": config["steps"]})
        sampled_run = self.observe(state, entry)
        sample = sampled_run["metrics_sample"]
        require(type(sample) is dict and type(sample["steps"]) is int and sample["steps"] == config["steps"]
                and type(sample["simulated_time_ns"]) is int and sample["simulated_time_ns"] == run["simulated_time_ns"],
                "Numerical sample is stale")
        total, _ = topology(config["cloth"])
        particles = []
        for offset in range(0, total, 256):
            page = self.call("simulation.particles", {**identity, "offset": offset, "limit": 256})
            length = min(256, total-offset)
            require(page["run_id"] == run["run_id"] and type(page["steps"]) is int and page["steps"] == config["steps"]
                    and type(page["simulated_time_ns"]) is int and page["simulated_time_ns"] == run["simulated_time_ns"]
                    and type(page["offset"]) is int and page["offset"] == offset
                    and type(page["total"]) is int and page["total"] == total
                    and type(page["has_more"]) is bool and page["has_more"] == (offset+length < total)
                    and type(page["particles"]) is list and len(page["particles"]) == length,
                    "Particle page does not match the experiment boundary")
            for i, particle in enumerate(page["particles"], offset):
                keys(particle, ("index", "position", "velocity", "inverse_mass"))
                require(type(particle["index"]) is int and particle["index"] == i, "Particle indices are not contiguous")
                particles.append({k: v for k, v in particle.items() if k != "index"})
        final = self.observe(self.call("simulation.query"), entry)
        require(final["steps"] == config["steps"] and not final["task"]["batch_active"]
                and final["task"]["status"] == run["task"]["status"], "Run changed during numerical collection")
        performance = final["task"]["performance"]
        provenance = {**identity, "steps": config["steps"], "simulated_time_ns": run["simulated_time_ns"],
            **{k: run["source"][k] for k in ("document_id", "scene_id", "revision")},
            "width": config["view"]["width"], "height": config["view"]["height"],
            "performance": {**performance, "scope": "batch_submit_to_completion"} if performance is not None else None,
            "files": ["config.json", "metrics.json", "particles.json"]}
        contents = {"config.json": config, "metrics.json": sample, "particles.json": particles, "provenance.json": provenance}
        stage = Path(tempfile.mkdtemp(prefix=".numeric-", dir=self.root / "results"))
        try:
            for name, value in contents.items():
                with (stage / name).open("xb") as stream:
                    stream.write(canonical(value) + b"\n")
                    stream.flush()
                    os.fsync(stream.fileno())
            _inspect(self.root, entry, stage.relative_to(self.root).as_posix(), run["run_id"])
            self.budget()
            stage.rename(self.root / self.current["directory"])
        finally:
            if stage.exists():
                for name in contents:
                    (stage / name).unlink(missing_ok=True)
                stage.rmdir()

    def collect(self, entry, run):
        if entry["output"]["image"]:
            config = entry["config"]
            output = self.output + "/" + self.current["directory"]
            receipt = self.call("simulation.export", {"run_id": run["run_id"], "expected_steps": config["steps"],
                "output": output, "width": config["view"]["width"], "height": config["view"]["height"]})
            require(receipt["run_id"] == run["run_id"] and receipt["output"] == output
                    and type(receipt["steps"]) is int and receipt["steps"] == config["steps"]
                    and type(receipt["simulated_time_ns"]) is int and receipt["simulated_time_ns"] == run["simulated_time_ns"]
                    and receipt["files"] == ["config.json", "metrics.json", "particles.json", "image.ppm", "provenance.json"],
                    "Export receipt differs from the requested experiment")
        else:
            self.numeric(entry, run)
        evidence = _inspect(self.root, entry, self.current["directory"], run["run_id"])
        require(all(evidence["source"][k] == run["source"][k] for k in ("document_id", "scene_id", "revision")),
                "Artifact provenance differs from accepted scene source")
        self.budget()
        attempt = register_artifacts(self.root, entry["experiment_id"], self.current["directory"],
                                     expected_run_id=run["run_id"], execution_environment=self.observed)
        self.current["artifact_attempt_id"] = attempt["attempt_id"]
        self.save()

    def stop(self, entry):
        state = self.call("simulation.stop", {"run_id": self.current["run_id"]})
        while state != {"mode": "edit", "run": None}:
            self.observe(state, entry)
            require(state["mode"] == "stopping", "Stop did not enter stopping or edit")
            time.sleep(min(self.poll_interval, self.budget()))
            state = self.call("simulation.query")

    def execute(self):
        try:
            self.save()
            for item, entry in zip(self.report["experiments"], self.index["experiments"]):
                self.current = item
                self.deadline = time.monotonic() + self.timeout
                state = self.call("scene.query")["state"]
                require(canonical(state) == canonical(self.report["source"]), "Edit source changed during batch")
                require(self.call("simulation.query") == {"mode": "edit", "run": None}, "Runtime is not idle")
                item["status"] = "submitting"
                config = entry["config"]
                state = self.call("simulation.run", {"guard": guard(self.report["source"]), "count": max(1, config["steps"]),
                    "paused": config["steps"] == 0, "solver": config["solver"], "cloth": config["cloth"],
                    "fixed_dt_ns": config["fixed_dt_ns"], "batch_steps": 8})
                item["submission"] = clone_json(item["last_call"])
                item["run_id"] = state["run"]["run_id"]
                _uuid(item["run_id"])
                item["status"] = "running"
                self.save()
                run = self.settled(state, entry)
                item["status"] = "collecting"
                self.save()
                self.collect(entry, run)
                item["status"] = "stopping"
                self.save()
                self.stop(entry)
                item["status"] = "succeeded"
                self.save()
            self.report["status"] = "succeeded"
            self.save()
            return self.report
        except (Exception, KeyboardInterrupt) as error:
            unknown = isinstance(error, (_Unconfirmed, KeyboardInterrupt)) or (isinstance(error, TransportError) and error.execution == "unknown")
            status = "unknown" if unknown else "cancelled" if isinstance(error, _Cancelled) else "failed"
            details = {"type": type(error).__name__, "message": str(error)[:4096]}
            self.report.update(status=status, error=details)
            if self.current is not None and self.current["status"] != "succeeded":
                self.current.update(status=status, error=details)
            recording_error = None
            try:
                self.save(notify=False)
            except Exception as failure:
                recording_error = str(failure)
            raise ExperimentBatchError(error, self.root, self.report, recording_error) from error


def run_plan(client, plan, *, project_root, output, experiment_timeout=600, poll_interval=.05,
             runner_executable=None, progress=None):
    """Execute a new plan serially; preserve partial work on error. No resume or retries."""
    expanded = expand_plan(plan)
    require(type(experiment_timeout) in (int, float) and .01 <= experiment_timeout <= 86400, "Experiment timeout must be .01..86400 seconds")
    require(type(poll_interval) in (int, float) and .001 <= poll_interval <= 5, "Poll interval must be .001..5 seconds")
    require(progress is None or callable(progress), "progress must be callable")
    parts = _relative_parts(output)
    require(len(output.encode("utf-8")) <= 800, "Output path is too long for simulation.export")
    with _errors():
        project = _root(project_root)
        if len(parts) > 1:
            require(_inside(project, "/".join(parts[:-1])).is_dir(), "Output parent must exist")
        root = project / output
        require(not os.path.lexists(root), "Batch output already exists; resume is not implemented")
        hello = client.hello()
        session = hello["session"]
        with _runtime_lease(session):
            def preflight(method):
                reply = client.call(method)
                require(reply.ticket.session == session, "Runtime changed during preflight")
                return reply.value
            caps = preflight("runtime.capabilities")
            simulation = caps.get("simulation", {})
            require(simulation.get("finite_tasks") is True and simulation.get("solver") == "xpbd_cpu",
                    "Runtime lacks finite XPBD tasks")
            for entry in expanded["experiments"]:
                require(entry["config"]["solver"] != "xpbd_gpu" or simulation.get("gpu") is True, "Plan requires GPU simulation")
                require(not entry["output"]["image"] or simulation.get("experiment_export") is True, "Plan requires image export")
            source = preflight("scene.query")["state"]
            guard(source)
            require(preflight("simulation.query") == {"mode": "edit", "run": None}, "Runtime has an existing PlayWorld")
            observed = environment(executable=runner_executable, runtime=caps)
            index = create_index(root, expanded["plan"], execution_environment=observed)
            (root / "results").mkdir()
            return _Runner(client, index, root, output, source, session, observed,
                           experiment_timeout, poll_interval, progress).execute()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="action", required=True)
    run = subparsers.add_parser("run", help="Execute a plan into a new index directory")
    run.add_argument("plan", type=Path)
    run.add_argument("--pipe", required=True)
    run.add_argument("--ctl", default="dk-ctl")
    run.add_argument("--project-root", required=True, type=Path)
    run.add_argument("--output", required=True)
    run.add_argument("--runner", type=Path, help="Optional host executable fingerprint")
    run.add_argument("--call-timeout", type=float, default=60)
    run.add_argument("--experiment-timeout", type=float, default=600)
    run.add_argument("--poll-interval", type=float, default=.05)
    status = subparsers.add_parser("status", help="Inspect durable progress and artifact bindings")
    status.add_argument("directory", type=Path)
    args = parser.parse_args()
    try:
        if args.action == "status":
            print(json.dumps(read_progress(args.directory), ensure_ascii=False))
        else:
            report = run_plan(Client(args.pipe, args.ctl, timeout=args.call_timeout), read_json(args.plan, PLAN_LIMIT),
                project_root=args.project_root, output=args.output, experiment_timeout=args.experiment_timeout,
                poll_interval=args.poll_interval, runner_executable=args.runner,
                progress=lambda event: print(json.dumps(event, ensure_ascii=False), file=sys.stderr, flush=True))
            print(json.dumps(_summary(report), ensure_ascii=False))
    except ExperimentBatchError as error:
        print(json.dumps({**_summary(error.progress), "error": str(error)}, ensure_ascii=False))
        parser.exit(1)
    except (ExperimentError, RpcError, TransportError, ValueError) as error:
        parser.exit(2, f"{error}\n")


if __name__ == "__main__":
    main()
