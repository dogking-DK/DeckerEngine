"""Real batch process termination, restart, publication adoption and explicit retries."""

import argparse
import copy
import json
from pathlib import Path
import subprocess
import sys
import time
from unittest.mock import patch
from uuid import uuid4

from decker import CallTimeout, Client
from decker.experiment_batch import ExperimentBatchError, read_progress, run_plan
from decker.experiment_index import verify_index
from decker.experiments import canonical, read_json
from PythonClientTest import server


def child(args):
    root = Path(args.work_dir)
    client = Client(args.endpoint, args.client, timeout=60)
    def barrier():
        (root / "barrier.json").write_bytes(canonical(read_progress(root / args.output)))
        while True:
            time.sleep(.1)  # Parent kills this owned process at a durable, observable boundary.
    def event(value):
        item = value["current"]
        if args.boundary == "accepted" and item and item["status"] == "running" and value["counts"]["succeeded"] == 1:
            barrier()
    from decker.experiment_index import _publish, register_artifacts
    def register(*positional, **keywords):
        if args.boundary == "published":
            barrier()
        result = register_artifacts(*positional, **keywords)
        if args.boundary == "indexed":
            barrier()
        return result
    def publish(directory, value, filename="index.json"):
        if args.boundary == "index-lock" and any(e["attempts"] for e in value["experiments"]):
            barrier()
        return _publish(directory, value, filename)
    original_call = client.call
    def call(method, params=None, **options):
        result = original_call(method, params, **options)
        if args.boundary == "stopped" and method == "simulation.stop":
            barrier()
        return result
    client.call = call
    with patch("decker.experiment_batch.register_artifacts", side_effect=register), \
         patch("decker.experiment_index._publish", side_effect=publish):
        run_plan(client, read_json(root / "plan.json"), project_root=root, output=args.output,
                 poll_interval=.01, progress=event, experiment_timeout=90)
    raise AssertionError("Child did not reach crash boundary")


def snapshot_files(root):
    return {str(p.relative_to(root)): p.read_bytes() for p in root.glob("results/**/*") if p.is_file()}


def acceptance(args):
    root = Path(args.work_dir).resolve() / uuid4().hex
    root.mkdir(parents=True)
    endpoint = "dk-recovery-" + uuid4().hex
    args.pipe_timeout_ms = 60000
    plan = {"format": "DeckerExperimentPlan", "version": 1,
        "base": {"steps": 9, "fixed_dt_ns": 10000000, "cloth": {"columns": 4, "rows": 4, "iterations": 2}},
        "sweep": {}, "seeds": [42, 43, 44], "solvers": ["xpbd_gpu" if args.gpu else "xpbd_cpu"],
        "output": {"image": args.gpu, "width": 96, "height": 72},
        "budget": {"max_experiments": 8, "max_total_steps": 100000, "max_work_units": 10**10}}
    (root / "plan.json").write_bytes(canonical(plan))
    outcomes = []
    with server(args, root, endpoint) as client:
        client.timeout = 60
        source = client.call("scene.new", {"name": "M13.3 recovery"}).value
        history = client.call("history.status").value
        def resume(output, retry=False, expected=0):
            command = [sys.executable, "-m", "decker.experiment_batch", "resume", "--pipe", endpoint,
                "--ctl", args.client, "--project-root", str(root), "--output", output, "--poll-interval", ".01"]
            if retry:
                command.append("--retry-failed")
            completed = subprocess.run(command, capture_output=True, timeout=120, creationflags=subprocess.CREATE_NO_WINDOW)
            (root / f"{output}-resume-{uuid4().hex[:8]}.log").write_bytes(completed.stdout + completed.stderr)
            assert completed.returncode == expected, completed.stdout.decode() + completed.stderr.decode()
            return read_progress(root / output)
        boundaries = ("accepted", "published", "indexed", "stopped", "index-lock")
        for boundary in boundaries:
            (root / "barrier.json").unlink(missing_ok=True)
            command = [sys.executable, __file__, "--child", "--client", args.client, "--runner", args.runner,
                "--work-dir", str(root), "--endpoint", endpoint, "--output", boundary, "--boundary", boundary]
            with (root / f"{boundary}-child.log").open("wb") as log:
                process = subprocess.Popen(command, stdout=log, stderr=log, creationflags=subprocess.CREATE_NO_WINDOW)
                try:
                    deadline = time.monotonic() + 90
                    while not (root / "barrier.json").exists():
                        assert process.poll() is None, f"Child failed; see {boundary}-child.log"
                        assert time.monotonic() < deadline, "Crash boundary timed out"
                        time.sleep(.01)
                    before = read_progress(root / boundary)
                    saved = snapshot_files(root / boundary)
                    if boundary == "accepted":
                        original = (root / boundary / "batch.json").read_bytes()
                        resume(boundary, expected=2)  # Live writer lease must reject another process.
                        assert original == (root / boundary / "batch.json").read_bytes()
                    process.kill()
                    process.wait(timeout=10)
                finally:
                    if process.poll() is None:
                        process.kill()
                        process.wait(timeout=10)
            after = resume(boundary)
            index = verify_index(root / boundary)
            assert after["status"] == "succeeded" and all(not e["history"] for e in after["experiments"])
            for previous, current in zip(before["experiments"], after["experiments"]):
                if previous["run_id"] is not None:
                    assert previous["run_id"] == current["run_id"]
            assert all(len(e["attempts"]) == 1 for e in index["experiments"])
            assert all((root / boundary / path).read_bytes() == data for path, data in saved.items())
            assert client.call("simulation.query").value == {"mode": "edit", "run": None}
            outcomes.append({"boundary": boundary, "batch_id": after["batch_id"], "preserved_files": len(saved)})

        # Real accepted command with its delivery deliberately lost; CLI must recover the same ticket/run.
        original_call = client.call
        lost = None
        def lose(method, params=None, **options):
            nonlocal lost
            reply = original_call(method, params, **options)
            if method == "simulation.run" and lost is None:
                lost = reply
                raise CallTimeout("injected reply loss after real acceptance", status="timeout", execution="unknown", ticket=reply.ticket)
            return reply
        with patch.object(client, "call", side_effect=lose):
            try:
                run_plan(client, plan, project_root=root, output="lost", poll_interval=.01)
                raise AssertionError("Lost reply succeeded")
            except ExperimentBatchError as error:
                assert error.progress["status"] == "unknown"
        recovered = resume("lost")
        assert recovered["experiments"][0]["run_id"] == lost.value["run"]["run_id"]
        assert recovered["experiments"][0]["submission"]["task_id"] == lost.task_id

        # Retry a local failure after artifact registration, preserving the original successful evidence.
        def fail(event):
            if event["current"] and event["current"]["artifact_attempt_id"]:
                raise OSError("injected observer failure after artifact commit")
        try:
            run_plan(client, plan, project_root=root, output="failed", progress=fail, poll_interval=.01)
            raise AssertionError("Failure was hidden")
        except ExperimentBatchError as error:
            assert error.progress["status"] == "failed"
        saved = snapshot_files(root / "failed")
        resume("failed", expected=1)
        retried = resume("failed", retry=True)
        first = retried["experiments"][0]
        assert first["history"][0]["status"] == "failed" and first["run_id"] != first["history"][0]["run_id"]
        assert all((root / "failed" / path).read_bytes() == data for path, data in saved.items())
        assert len(verify_index(root / "failed")["experiments"][0]["attempts"]) == 2

        # Zero-step finite tasks stay paused after initialization, allowing deterministic external cancellation.
        cancelled_plan = copy.deepcopy(plan)
        cancelled_plan["base"]["steps"] = 0
        cancelled = None
        def cancel(event):
            nonlocal cancelled
            item = event["current"]
            if item and item["status"] == "running" and cancelled is None:
                cancelled = item["run_id"]
                client.call("simulation.cancel", {"run_id": cancelled})
        try:
            run_plan(client, cancelled_plan, project_root=root, output="cancelled", progress=cancel, poll_interval=.01)
            raise AssertionError("Cancelled batch succeeded")
        except ExperimentBatchError as error:
            assert error.progress["status"] == "cancelled", error
        retained = resume("cancelled", expected=1)
        assert retained["status"] == "cancelled" and retained["experiments"][1]["status"] == "pending"
        retried = resume("cancelled", retry=True)
        assert retried["experiments"][0]["history"][0]["status"] == "cancelled"
        assert retried["experiments"][0]["history"][0]["run_id"] == cancelled
        assert client.call("simulation.query").value == {"mode": "edit", "run": None}
        assert client.call("scene.query").value["state"] == source and client.call("history.status").value == history
        (root / "report.json").write_bytes(canonical({"gpu": args.gpu, "boundaries": outcomes,
            "same_ticket": True, "explicit_retry_preserved_evidence": True, "cancelled_retained": True}))
    print(f"Recovery process kills/restarts, ticket replay, retry and cancellation passed: {root}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("runner", "client", "work-dir"):
        parser.add_argument("--" + name, required=True)
    parser.add_argument("--gpu", action="store_true")
    parser.add_argument("--child", action="store_true")
    for name in ("endpoint", "output", "boundary"):
        parser.add_argument("--" + name)
    args = parser.parse_args()
    child(args) if args.child else acceptance(args)


if __name__ == "__main__":
    main()
