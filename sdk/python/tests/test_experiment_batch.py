"""Batch behavior tests with explicit transport/engine/file failure injection."""

import copy
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
from uuid import uuid4

from decker import CallTimeout, Reply, Ticket
from decker.experiments import ExperimentError, canonical, topology
from decker.experiment_index import METRICS, verify_index
from decker.experiment_batch import ExperimentBatchError, read_progress, run_plan, _runtime_lease
from test_experiments import plan


class FakeClient:
    endpoint = "test-batch"
    timeout = 1

    def __init__(self):
        self.session = str(uuid4())
        self.source = {"document_id": str(uuid4()), "scene_id": str(uuid4()), "revision": 0,
                       "dirty": True, "entity_count": 0}
        self.run = None
        self.calls = []
        self.request_id = 0
        self.hook = None
        self.frozen = False

    def hello(self):
        self.calls.append(("hello", {}))
        return {"protocol": 1, "session": self.session}

    def call(self, method, params=None, **kwargs):
        params = copy.deepcopy(params or {})
        self.calls.append((method, params))
        self.request_id += 1
        if method == "runtime.capabilities":
            value = {"simulation": {"finite_tasks": True, "solver": "xpbd_cpu", "gpu": False, "experiment_export": False}}
        elif method == "scene.query":
            value = {"state": copy.deepcopy(self.source)}
        elif method == "simulation.run":
            assert self.run is None, "overlapping engine task"
            self.run = {"run_id": str(uuid4()), "source": copy.deepcopy(self.source),
                "cloth": params["cloth"], "solver": params["solver"], "fixed_dt_ns": params["fixed_dt_ns"],
                "steps": 0, "simulated_time_ns": 0, "fault": None, "metrics_sample": None,
                "task": {"target_steps": params["count"], "submitted_steps": 0, "completed_steps": 0,
                    "in_flight_steps": 0, "batch_steps": 8, "batch_active": False, "initialized": True,
                    "status": "paused" if params["paused"] else "running", "performance": None}}
            value = self.state()
        elif method == "simulation.query":
            if self.run and self.run["task"]["status"] == "running" and not self.frozen:
                task = self.run["task"]
                count = min(8, task["target_steps"] - self.run["steps"])
                self.run["steps"] += count
                self.run["simulated_time_ns"] = self.run["steps"] * self.run["fixed_dt_ns"]
                task.update(submitted_steps=self.run["steps"], completed_steps=self.run["steps"],
                            performance={"steps": self.run["steps"], "batch_steps": count, "wall_time_ns": 1000})
                if self.run["steps"] == task["target_steps"]:
                    task["status"] = "succeeded"
            value = self.state()
        elif method == "simulation.sample":
            self.check_identity(params)
            particles, constraints = topology(self.run["cloth"])
            metrics = {field: 0.0 for field in METRICS}
            metrics.update(particle_count=particles, constraint_count=constraints, color_count=8)
            self.run["metrics_sample"] = {"steps": self.run["steps"], "simulated_time_ns": self.run["simulated_time_ns"], "metrics": metrics}
            value = self.state()
        elif method == "simulation.particles":
            self.check_identity(params)
            total, _ = topology(self.run["cloth"])
            end = min(total, params["offset"] + params["limit"])
            value = {"run_id": self.run["run_id"], "steps": self.run["steps"], "simulated_time_ns": self.run["simulated_time_ns"],
                "offset": params["offset"], "total": total, "has_more": end < total,
                "particles": [{"index": i, "position": [0, 1, 0], "velocity": [0, 0, 0], "inverse_mass": 1}
                              for i in range(params["offset"], end)]}
        elif method == "simulation.stop":
            self.check_identity(params)
            self.run = None
            value = self.state()
        else:
            raise AssertionError(method)
        if self.hook:
            self.hook(method, value)
        return Reply({"session": self.session, "request_id": self.request_id,
                      "response": {"result": {"value": value, "task_id": str(uuid4()), "status": "succeeded"}}})

    def check_identity(self, params):
        assert self.run is not None and params["run_id"] == self.run["run_id"]

    def state(self):
        mode = "edit" if self.run is None else "running" if self.run["task"]["status"] == "running" else "paused"
        return {"mode": mode, "run": copy.deepcopy(self.run)}


class BatchTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.client = FakeClient()
        self.plan = plan()
        self.plan["seeds"] = [42, 43]

    def execute(self, **kwargs):
        return run_plan(self.client, self.plan, project_root=self.root, output="batch", poll_interval=.001, **kwargs)

    def methods(self):
        return [method for method, _ in self.client.calls]

    def test_serial_zero_partial_batch_multipage_and_durable_progress(self):
        self.plan["sweep"] = {"steps": [0, 9]}
        self.plan["base"]["cloth"] = {"columns": 17, "rows": 16}
        events = []

        def progress(event):
            saved = read_progress(self.root / "batch")
            self.assertEqual(saved["sequence"], event["sequence"])
            events.append(event)
            # Callbacks receive detached snapshots.
            event["counts"]["succeeded"] = -1

        report = self.execute(progress=progress)
        self.assertEqual(report["status"], "succeeded")
        self.assertEqual(read_progress(self.root / "batch"), report)
        self.assertEqual([e["completed_steps"] for e in report["experiments"]], [0, 0, 9, 9])
        self.assertEqual(len({e["run_id"] for e in report["experiments"]}), 4)
        calls = self.methods()
        self.assertEqual([m for m in calls if m in ("simulation.run", "simulation.stop")],
                         ["simulation.run", "simulation.stop"] * 4)
        for e in report["experiments"]:
            self.assertIsNotNone(e["artifact_attempt_id"])
            self.assertEqual(e["submission"]["ticket"]["session"], self.client.session)
        self.assertIsNone(self.client.run)
        self.assertTrue(any(e["current"] and e["current"]["task_status"] == "running" for e in events))
        self.assertEqual(len([p for m, p in self.client.calls if m == "simulation.particles" and p["offset"] == 256]), 4)
        self.assertFalse(list((self.root / "batch/results").glob(".numeric-*")))

    def test_invalid_plan_capability_busy_and_output_reject_before_mutation(self):
        self.plan["budget"]["max_experiments"] = 1
        with self.assertRaises(ExperimentError): self.execute()
        self.assertEqual(self.client.calls, [])
        self.plan["budget"]["max_experiments"] = 2
        self.plan["solvers"] = ["xpbd_gpu"]
        with self.assertRaises(ExperimentError): self.execute()
        self.assertNotIn("simulation.run", self.methods())
        self.assertFalse((self.root / "batch").exists())
        self.plan["solvers"] = ["xpbd_cpu"]
        (self.root / "batch").mkdir()
        calls = list(self.client.calls)
        with self.assertRaises(ExperimentError): self.execute()
        self.assertEqual(calls, self.client.calls)

    def test_runtime_lease_rejects_same_thread_recursion_without_output(self):
        with _runtime_lease(self.client.session):
            with self.assertRaises(ExperimentError): self.execute()
        self.assertFalse((self.root / "batch").exists())
        self.assertNotIn("simulation.run", self.methods())

    def test_explicit_failed_and_cancelled_never_collect_or_start_next(self):
        for status in ("failed", "cancelled"):
            with self.subTest(status=status), tempfile.TemporaryDirectory(dir=self.root) as root:
                client = FakeClient()
                def failure(method, value):
                    if method == "simulation.run":
                        value["run"]["task"]["status"] = status
                        value["run"]["fault"] = "internal_error" if status == "failed" else None
                client.hook = failure
                with self.assertRaises(ExperimentBatchError):
                    run_plan(client, self.plan, project_root=root, output="batch")
                report = read_progress(Path(root) / "batch")
                self.assertEqual([e["status"] for e in report["experiments"]], [status, "pending"])
                self.assertEqual(report["status"], status)
                self.assertEqual(sum(m == "simulation.run" for m, p in client.calls), 1)
                self.assertFalse(any(m in ("simulation.stop", "simulation.sample") for m, p in client.calls))

    def test_ambiguous_submission_preserves_ticket_without_retry_or_cancel(self):
        def timeout(method, value):
            if method == "simulation.run":
                raise CallTimeout("accepted but response lost", status="timeout", execution="unknown",
                                  ticket=Ticket(self.client.session, self.client.request_id))
        self.client.hook = timeout
        with self.assertRaises(ExperimentBatchError) as caught: self.execute()
        self.assertEqual(caught.exception.progress["status"], "unknown")
        report = read_progress(self.root / "batch")
        item = report["experiments"][0]
        self.assertEqual(item["last_call"]["ticket"]["session"], self.client.session)
        self.assertIsNone(item["run_id"])
        self.assertEqual(self.methods().count("simulation.run"), 1)
        self.assertNotIn("simulation.stop", self.methods())
        self.assertIsNotNone(self.client.run)

    def test_deadline_keeps_active_run_and_pending_tail(self):
        self.client.frozen = True
        with self.assertRaises(ExperimentBatchError): self.execute(experiment_timeout=.1)
        report = read_progress(self.root / "batch")
        self.assertEqual(report["status"], "unknown")
        self.assertEqual(report["experiments"][1]["status"], "pending")
        self.assertNotIn("simulation.stop", self.methods())
        self.assertIsNotNone(self.client.run)

    def test_stale_sample_and_cross_run_pages_never_publish_success(self):
        for field in ("sample", "page"):
            with self.subTest(field=field), tempfile.TemporaryDirectory(dir=self.root) as root:
                client = FakeClient()
                def corrupt(method, value):
                    if field == "sample" and method == "simulation.sample":
                        value["run"]["metrics_sample"]["steps"] = 0
                    if field == "page" and method == "simulation.particles":
                        value["run_id"] = str(uuid4())
                client.hook = corrupt
                with self.assertRaises(ExperimentBatchError):
                    run_plan(client, self.plan, project_root=root, output="batch", poll_interval=.001)
                report = read_progress(Path(root) / "batch")
                self.assertEqual(report["status"], "failed")
                self.assertTrue(all(not e["attempts"] for e in verify_index(Path(root) / "batch")["experiments"]))
                self.assertFalse(any(m == "simulation.stop" for m, _ in client.calls))

    def test_registration_failure_preserves_published_numeric_files(self):
        with patch("decker.experiment_batch.register_artifacts", side_effect=OSError("disk full")):
            with self.assertRaises(ExperimentBatchError): self.execute()
        report = read_progress(self.root / "batch")
        first = report["experiments"][0]
        self.assertTrue((self.root / "batch" / first["directory"] / "metrics.json").is_file())
        self.assertIsNone(first["artifact_attempt_id"])
        self.assertEqual(first["status"], "failed")
        self.assertNotIn("simulation.stop", self.methods())

    def test_stop_timeout_keeps_verified_artifact_but_batch_unknown(self):
        def timeout(method, value):
            if method == "simulation.stop":
                raise CallTimeout("Stop response lost", status="timeout", execution="unknown")
        self.client.hook = timeout
        with self.assertRaises(ExperimentBatchError): self.execute()
        report = read_progress(self.root / "batch")
        self.assertEqual(report["status"], "unknown")
        self.assertIsNotNone(report["experiments"][0]["artifact_attempt_id"])
        self.assertEqual(verify_index(self.root / "batch")["experiments"][0]["status"], "verified")
        self.assertEqual(self.methods().count("simulation.run"), 1)

    def test_source_change_after_success_keeps_first_artifact_and_stops(self):
        def edit(method, value):
            if method == "simulation.stop":
                self.client.source["revision"] += 1
        self.client.hook = edit
        with self.assertRaises(ExperimentBatchError): self.execute()
        report = read_progress(self.root / "batch")
        self.assertEqual([e["status"] for e in report["experiments"]], ["succeeded", "failed"])
        self.assertEqual(self.methods().count("simulation.run"), 1)

    def test_progress_write_failure_retains_last_snapshot_and_does_not_continue(self):
        from decker.experiment_batch import _publish
        def fail(root, report, filename):
            if any(e["status"] in ("running", "failed") for e in report["experiments"]):
                raise OSError("injected progress failure")
            return _publish(root, report, filename)
        with patch("decker.experiment_batch._publish", side_effect=fail):
            with self.assertRaises(ExperimentBatchError) as caught: self.execute()
        self.assertIsNotNone(caught.exception.recording_error)
        report = read_progress(self.root / "batch")
        self.assertEqual(report["experiments"][0]["status"], "submitting")
        self.assertEqual(report["experiments"][0]["last_call"]["execution"], "unknown")
        self.assertEqual(self.methods().count("simulation.run"), 1)
        self.assertNotIn("simulation.stop", self.methods())

    def test_callback_failure_stops_and_reports_without_deleting_evidence(self):
        def fail(event):
            if event["current"] and event["current"]["status"] == "collecting":
                raise RuntimeError("observer failed")
        with self.assertRaises(ExperimentBatchError): self.execute(progress=fail)
        self.assertEqual(read_progress(self.root / "batch")["status"], "failed")
        self.assertEqual(self.methods().count("simulation.run"), 1)
        self.assertIsNotNone(self.client.run)

    def test_keyboard_interrupt_and_changed_session_are_unconfirmed(self):
        for interrupt in (True, False):
            with self.subTest(interrupt=interrupt), tempfile.TemporaryDirectory(dir=self.root) as root:
                client = FakeClient()
                def disrupt(method, value):
                    if method == "simulation.run":
                        if interrupt:
                            raise KeyboardInterrupt()
                        client.session = str(uuid4())
                client.hook = disrupt
                with self.assertRaises(ExperimentBatchError):
                    run_plan(client, self.plan, project_root=root, output="batch")
                self.assertEqual(read_progress(Path(root) / "batch")["status"], "unknown")
                self.assertFalse(any(m == "simulation.stop" for m, _ in client.calls))

    def test_status_rejects_success_without_bound_attempt(self):
        self.execute()
        file = self.root / "batch/batch.json"
        value = json.loads(file.read_bytes())
        value["experiments"][0]["artifact_attempt_id"] = str(uuid4())
        file.write_bytes(canonical(value))
        with self.assertRaises(ExperimentError): read_progress(self.root / "batch")


if __name__ == "__main__":
    unittest.main()
