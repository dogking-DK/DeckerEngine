"""Recovery must reconcile ownership and publication boundaries before scheduling work."""

import copy
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
from uuid import uuid4

from decker import CallTimeout, RpcError, TransportError
from decker.experiment_batch import ExperimentBatchError, read_progress, resume_plan, run_plan
from decker.experiment_index import register_artifacts, verify_index
from decker.experiments import ExperimentError, canonical
from decker._experiment_lock import exclusive_path
import test_experiment_batch as fixture
from test_experiments import plan


class RecoveryClient(fixture.FakeClient):
    def __init__(self):
        super().__init__()
        self.receipts = {}
        self.lose_reply = False
        self.no_ticket = False
        self.expired = False

    def call(self, method, params=None, **kwargs):
        retry = kwargs.get("retry")
        if retry:
            self.calls.append(("ticket.replay", copy.deepcopy(params)))
            if self.expired:
                raise RpcError({"session": retry.session, "request_id": retry.request_id,
                    "response": {"error": {"code": -32071, "message": "Ticket expired", "data": None}}})
            receipt = self.receipts[retry.request_id]
            assert params == receipt[0]
            return receipt[1]
        reply = super().call(method, params, **kwargs)
        if method == "simulation.run":
            self.receipts[reply.ticket.request_id] = (copy.deepcopy(params), reply)
            if self.lose_reply:
                self.lose_reply = False
                raise CallTimeout("response lost", status="timeout", execution="unknown",
                                  ticket=None if self.no_ticket else reply.ticket)
        return reply


class RecoveryTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.client = RecoveryClient()
        self.plan = plan()
        self.plan["seeds"] = [42, 43]

    def start(self, **kwargs):
        return run_plan(self.client, self.plan, project_root=self.root, output="batch", poll_interval=.001, **kwargs)

    def resume(self, **kwargs):
        return resume_plan(self.client, project_root=self.root, output="batch", poll_interval=.001, **kwargs)

    def read(self):
        return read_progress(self.root / "batch")

    def runs(self):
        return sum(m == "simulation.run" for m, _ in self.client.calls)

    def crash(self, status, *, ordinal=0):
        ids = []
        def kill(event):
            item = event["current"]
            if item and item["experiment_id"] not in ids:
                ids.append(item["experiment_id"])
            if item and len(ids) == ordinal+1 and item["status"] == status:
                raise SystemExit("abrupt process exit")
        with self.assertRaises(SystemExit):
            self.start(progress=kill)

    def test_restart_skips_success_and_rejoins_accepted_run(self):
        self.crash("running", ordinal=1)
        before = self.read()
        first = copy.deepcopy(verify_index(self.root / "batch")["experiments"][0])
        result = self.resume()
        self.assertEqual(result["status"], "succeeded")
        self.assertEqual(self.runs(), 2)
        self.assertEqual([e["run_id"] for e in result["experiments"]], [e["run_id"] for e in before["experiments"]])
        self.assertEqual(first, verify_index(self.root / "batch")["experiments"][0])
        self.assertTrue(all(not e["history"] for e in result["experiments"]))

    def test_completed_batch_is_verified_noop_without_host(self):
        before = self.start()
        saved = (self.root / "batch/batch.json").read_bytes()
        self.client.hello = lambda: self.fail("Completed resume contacted host")
        self.assertEqual(self.resume(), before)
        self.assertEqual(saved, (self.root / "batch/batch.json").read_bytes())

    def test_unknown_submission_replays_only_original_ticket(self):
        self.client.lose_reply = True
        with self.assertRaises(ExperimentBatchError): self.start()
        request = self.read()["experiments"][0]["request"]
        accepted = self.client.run["run_id"]
        result = self.resume()
        self.assertEqual(self.runs(), 2)
        self.assertEqual(result["experiments"][0]["run_id"], accepted)
        self.assertEqual(result["experiments"][0]["submission"]["ticket"], request["ticket"])
        self.assertEqual(sum(m == "ticket.replay" for m, _ in self.client.calls), 1)

    def test_unsent_reconciliation_does_not_mark_original_request_unsent(self):
        self.client.lose_reply = True
        with self.assertRaises(ExperimentBatchError): self.start()
        original_id = self.client.run["run_id"]
        original_call = self.client.call
        def fail(method, params=None, **options):
            if options.get("retry"):
                raise TransportError("client could not launch", status="launch_error", execution="not_sent")
            return original_call(method, params, **options)
        with patch.object(self.client, "call", side_effect=fail), self.assertRaises(ExperimentBatchError):
            self.resume(retry_failed=True)
        report = self.read()
        self.assertEqual(report["status"], "unknown")
        self.assertEqual(report["experiments"][0]["request"]["execution"], "unknown")
        def broken_observer(event):
            if event["current"] and event["current"]["last_call"]["method"] == "simulation.run":
                raise RuntimeError("observer interrupted ticket reconciliation")
        with self.assertRaises(ExperimentBatchError): self.resume(progress=broken_observer)
        self.assertEqual(self.read()["status"], "unknown")
        result = self.resume()
        self.assertEqual(result["experiments"][0]["run_id"], original_id)
        self.assertEqual(self.runs(), 2)

    def test_missing_or_expired_ticket_remains_unknown_even_with_retry(self):
        for no_ticket in (True, False):
            with self.subTest(no_ticket=no_ticket), tempfile.TemporaryDirectory(dir=self.root) as root:
                client = RecoveryClient()
                client.lose_reply, client.no_ticket = True, no_ticket
                with self.assertRaises(ExperimentBatchError):
                    run_plan(client, self.plan, project_root=root, output="batch")
                client.expired = True
                with self.assertRaises(ExperimentBatchError) as caught:
                    resume_plan(client, project_root=root, output="batch", retry_failed=True)
                self.assertEqual(caught.exception.progress["status"], "unknown")
                self.assertEqual(sum(m == "simulation.run" for m, _ in client.calls), 1)
                self.assertFalse(any(m == "simulation.stop" for m, _ in client.calls))

    def test_timeout_rejoins_run_without_new_attempt(self):
        self.client.frozen = True
        with self.assertRaises(ExperimentBatchError): self.start(experiment_timeout=.05)
        run_id = self.client.run["run_id"]
        self.client.frozen = False
        result = self.resume()
        self.assertEqual(self.runs(), 2)
        self.assertEqual(result["experiments"][0]["run_id"], run_id)

    def test_cancelled_and_failed_require_explicit_retry_and_keep_history(self):
        self.crash("running")
        old_id = self.client.run["run_id"]
        self.client.run["task"]["status"] = "cancelled"
        with self.assertRaises(ExperimentBatchError): self.resume()
        self.assertEqual(self.read()["experiments"][0]["status"], "cancelled")
        result = self.resume(retry_failed=True)
        item = result["experiments"][0]
        self.assertEqual(result["status"], "succeeded")
        self.assertEqual(item["history"][0]["status"], "cancelled")
        self.assertEqual(item["history"][0]["run_id"], old_id)
        self.assertNotEqual(item["run_id"], old_id)
        self.assertTrue(item["directory"].endswith("-retry-2"))
        self.assertEqual(self.runs(), 3)

    def test_local_failure_retry_retains_original_artifact_bytes_and_index(self):
        def fail(event):
            if event["current"] and event["current"]["artifact_attempt_id"]:
                raise RuntimeError("observer failed after index commit")
        with self.assertRaises(ExperimentBatchError): self.start(progress=fail)
        old = self.read()["experiments"][0]
        directory = self.root / "batch" / old["directory"]
        files = {p.name: p.read_bytes() for p in directory.iterdir()}
        with self.assertRaises(ExperimentBatchError): self.resume()
        self.assertEqual(self.runs(), 1)
        result = self.resume(retry_failed=True)
        self.assertEqual(files, {p.name: p.read_bytes() for p in directory.iterdir()})
        attempts = verify_index(self.root / "batch")["experiments"][0]["attempts"]
        self.assertEqual(len(attempts), 2)
        self.assertEqual(result["experiments"][0]["history"][0]["artifact_attempt_id"], old["artifact_attempt_id"])

    def test_publication_crash_adopts_files_and_index_without_rerun(self):
        for after_index in (False, True):
            with self.subTest(after_index=after_index), tempfile.TemporaryDirectory(dir=self.root) as root:
                client = RecoveryClient()
                def crash(*args, **kwargs):
                    if after_index:
                        register_artifacts(*args, **kwargs)
                    raise SystemExit("process died at publication")
                with patch("decker.experiment_batch.register_artifacts", side_effect=crash), self.assertRaises(SystemExit):
                    run_plan(client, self.plan, project_root=root, output="batch")
                before = read_progress(Path(root) / "batch")
                self.assertIsNone(before["experiments"][0]["artifact_attempt_id"])
                result = resume_plan(client, project_root=root, output="batch", poll_interval=.001)
                self.assertEqual(result["status"], "succeeded")
                self.assertEqual(sum(m == "simulation.run" for m, _ in client.calls), 2)
                self.assertEqual(len(verify_index(Path(root) / "batch")["experiments"][0]["attempts"]), 1)

    def test_stop_response_loss_uses_artifacts_and_idle_state(self):
        def crash(method, value):
            if method == "simulation.stop":
                raise SystemExit("stopped but no response")
        self.client.hook = crash
        with self.assertRaises(SystemExit): self.start()
        self.assertIsNone(self.client.run)
        self.client.hook = None
        self.assertEqual(self.resume()["status"], "succeeded")
        self.assertEqual(self.runs(), 2)

    def test_known_run_missing_without_artifacts_requires_explicit_retry(self):
        self.crash("running")
        self.client.run = None
        with self.assertRaises(ExperimentBatchError): self.resume()
        self.assertEqual(self.read()["status"], "failed")
        self.assertEqual(self.resume(retry_failed=True)["status"], "succeeded")
        self.assertEqual(self.runs(), 3)

    def test_tampered_success_refused_before_host_or_disk_mutation(self):
        self.crash("running", ordinal=1)
        report = self.read()
        path = self.root / "batch" / report["experiments"][0]["directory"] / "metrics.json"
        path.write_bytes(b"{}")
        saved = (self.root / "batch/batch.json").read_bytes()
        calls = len(self.client.calls)
        with self.assertRaises(ExperimentError): self.resume(retry_failed=True)
        self.assertEqual(calls, len(self.client.calls))
        self.assertEqual(saved, (self.root / "batch/batch.json").read_bytes())

    def test_changed_session_source_or_foreign_run_never_stopped(self):
        self.crash("running")
        session = self.client.session
        saved = (self.root / "batch/batch.json").read_bytes()
        self.client.session = str(uuid4())
        with self.assertRaises(ExperimentError): self.resume(retry_failed=True)
        self.assertEqual(saved, (self.root / "batch/batch.json").read_bytes())
        self.client.session = session
        self.client.source["revision"] += 1
        with self.assertRaises(ExperimentError): self.resume(retry_failed=True)
        self.client.source["revision"] -= 1
        self.client.run["run_id"] = str(uuid4())
        with self.assertRaises(ExperimentBatchError): self.resume(retry_failed=True)
        self.assertEqual(self.read()["status"], "unknown")
        self.assertFalse(any(m == "simulation.stop" for m, _ in self.client.calls))

    def test_v1_upgrade_and_output_directory_lease(self):
        self.crash("running")
        report = self.read()
        report["version"] = 1
        for item in report["experiments"]:
            del item["history"], item["request"]
        path = self.root / "batch/batch.json"
        path.write_bytes(canonical(report))
        with exclusive_path("batch", self.root / "batch"):
            with self.assertRaises(ExperimentError): self.resume()
        self.assertEqual(json.loads(path.read_bytes())["version"], 1)
        result = self.resume()
        self.assertEqual(result["version"], 2)
        self.assertEqual(result["status"], "succeeded")

    def test_retry_publication_failure_keeps_old_attempt_and_does_not_run(self):
        self.crash("running")
        self.client.run["task"]["status"] = "failed"
        self.client.run["fault"] = "injected"
        with self.assertRaises(ExperimentBatchError): self.resume()
        from decker.experiment_batch import _publish
        def fail(root, report, filename):
            if report["experiments"][0]["history"]:
                raise OSError("disk full at retry publication")
            return _publish(root, report, filename)
        with patch("decker.experiment_batch._publish", side_effect=fail):
            with self.assertRaises(ExperimentBatchError): self.resume(retry_failed=True)
        self.assertEqual(self.runs(), 1)
        self.assertEqual(self.read()["experiments"][0]["history"], [])
        self.assertEqual(self.read()["experiments"][0]["status"], "failed")

    def test_later_query_failure_does_not_erase_confirmed_cancellation(self):
        self.crash("running")
        self.client.run["task"]["status"] = "cancelled"
        with self.assertRaises(ExperimentBatchError): self.resume()
        original = self.read()["experiments"][0]["error"]
        def lose(method, value):
            if method == "simulation.query":
                raise CallTimeout("query lost", status="timeout", execution="unknown")
        self.client.hook = lose
        with self.assertRaises(ExperimentBatchError): self.resume(retry_failed=True)
        self.assertEqual(self.read()["experiments"][0]["status"], "cancelled")
        self.assertEqual(self.read()["experiments"][0]["error"], original)
        self.client.hook = None
        with self.assertRaises(ExperimentBatchError): self.resume()
        self.assertEqual(self.runs(), 1)
        self.assertEqual(self.resume(retry_failed=True)["experiments"][0]["history"][0]["status"], "cancelled")

    def test_incomplete_orphan_is_retained_and_retry_uses_new_directory(self):
        with patch("decker.experiment_batch.register_artifacts", side_effect=SystemExit()), self.assertRaises(SystemExit):
            self.start()
        path = self.root / "batch" / self.read()["experiments"][0]["directory"] / "metrics.json"
        path.write_bytes(b"{broken")
        with self.assertRaises(ExperimentBatchError): self.resume()
        self.assertEqual(self.read()["status"], "failed")
        result = self.resume(retry_failed=True)
        self.assertEqual(result["status"], "succeeded")
        self.assertEqual(path.read_bytes(), b"{broken")
        self.assertEqual(result["experiments"][0]["history"][0]["status"], "failed")

    def test_stopping_to_edit_race_waits_without_resending_stop(self):
        self.crash("stopping")
        original = self.client.run["run_id"]
        self.client.run["task"]["status"] = "stopping"
        def complete_stop(method, value):
            if method == "simulation.query" and value["run"] and value["run"]["run_id"] == original:
                value["mode"] = "stopping"
                self.client.run = None  # Owner finishes Stop immediately after the returned snapshot.
        self.client.hook = complete_stop
        result = self.resume()
        self.assertEqual(result["status"], "succeeded")
        self.assertFalse(any(m == "simulation.stop" and p["run_id"] == original for m, p in self.client.calls))


if __name__ == "__main__":
    unittest.main()
