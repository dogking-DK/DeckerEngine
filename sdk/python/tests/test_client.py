import json
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import unittest
from unittest.mock import patch

from decker import (
    ArtifactError, BatchError, CallTimeout, Capture, Client, Command, JobError,
    Reply, RpcError, Ticket, TransportError, WaitTimeout, guard,
)
from decker.client import _decode

SESSION = "10000000-0000-4000-8000-000000000001"
TASK = "20000000-0000-4000-8000-000000000001"
JOB = "30000000-0000-4000-8000-000000000001"


def response(value=None):
    return {"status": "ok", "execution": "received", "session": SESSION, "request_id": 1,
            "response": {"jsonrpc": "2.0", "id": 1,
                         "result": {"task_id": TASK, "status": "succeeded", "value": value}}}


def decode(raw, code=0):
    return _decode(json.dumps(raw).encode(), b"", code, False, None)


def job(state="succeeded", result=None):
    return {"id": JOB, "state": state, "cancel_requested": state == "cancelled",
            "result": result, "error": {"code": 4, "message": "failed", "context": []} if state == "failed" else None}


class ClientTests(unittest.TestCase):
    def test_exact_values_and_rpc_errors(self):
        reply = decode(response({"中文": [None, True, -(2**63), 2**64 - 1]}))
        self.assertEqual(reply.value["中文"][-1], 2**64 - 1)
        self.assertEqual(reply.ticket, Ticket(SESSION, 1))
        self.assertEqual(reply.task_id, TASK)
        raw = response()
        raw["response"].pop("result")
        raw["response"]["error"] = {"code": -32007, "message": "stale", "data": {"task_id": TASK, "engine_name": "conflict"}}
        with self.assertRaises(RpcError) as caught:
            decode(raw, 1)
        self.assertEqual(caught.exception.task_id, TASK)
        self.assertEqual(caught.exception.code, -32007)
        self.assertEqual(caught.exception.data["engine_name"], "conflict")

    def test_transport_failures_preserve_ticket_and_execution(self):
        for execution in ("not_sent", "unknown"):
            raw = {"status": "timeout", "execution": execution, "session": SESSION, "request_id": 1}
            with self.assertRaises(CallTimeout) as caught:
                decode(raw, 3)
            self.assertEqual(caught.exception.ticket, Ticket(SESSION, 1))
            self.assertEqual(caught.exception.execution, execution)

    def test_invalid_responses_are_never_success(self):
        variants = []
        for field, value in (("id", True), ("id", 2), ("jsonrpc", "1.0"), ("result", {})):
            raw = response()
            raw["response"][field] = value
            variants.append(json.dumps(raw).encode())
        variants += [b"", b"{}\n{}", b'{"status":"ok","status":"ok"}', b'\xff', b'NaN',
                     json.dumps(response()).replace('"value": null', '"value": 1e400').encode()]
        for output in variants:
            with self.subTest(output=output), self.assertRaises(TransportError) as caught:
                _decode(output, b"diagnostic", 0, False, None)
            self.assertEqual(caught.exception.execution, "unknown")
        with self.assertRaises(TransportError):
            _decode(json.dumps(response()).encode(), b"", 0, False, Ticket(SESSION, 2))
        missing_ticket = response()
        del missing_ticket["session"]
        del missing_ticket["request_id"]
        with self.assertRaises(TransportError):
            _decode(json.dumps(missing_ticket).encode(), b"", 0, False, Ticket(SESSION, 1))

    def test_input_validation_before_launch(self):
        client = Client("unit")
        cyclic = {}; cyclic["self"] = cyclic
        bad = [{1: "key"}, {"n": float("nan")}, {"n": float("inf")}, {"n": 2**64},
               {"n": -(2**63)-1}, {"x": (1, 2)}, {"x": object()}, {"x": "\ud800"},
               {"x": "x" * 1048576}, {"x": ["x" * 65536] * 32}, cyclic, []]
        with patch("decker.client.subprocess.Popen") as launch:
            for params in bad:
                with self.subTest(params_type=type(params)), self.assertRaises(ValueError):
                    client.call("scene.new", params)
            for timeout in (0, -1, 61, True, 2**1024, float("inf"), float("nan")):
                with self.assertRaises(ValueError):
                    client.call("scene.new", timeout=timeout)
            with self.assertRaises(ValueError):
                client.batch([Command("scene.new"), Command("bad", {"x": object()})])
            launch.assert_not_called()
        for value in (0, True, 2**63):
            with self.assertRaises(ValueError):
                Ticket(SESSION, value)
        with self.assertRaises(ValueError):
            Client("bad pipe")
        self.assertEqual(guard({"document_id": SESSION, "revision": 2**64-1})["revision"], 2**64-1)

    def test_launch_error_is_not_sent(self):
        with self.assertRaises(TransportError) as caught:
            Client("unit", "missing-decker-executable-unique.exe").call("scene.new")
        self.assertEqual(caught.exception.execution, "not_sent")

    def test_real_process_timeout_is_reaped_and_temp_removed(self):
        original = subprocess.Popen
        children, files = [], []

        def launch(args, **kwargs):
            files.append(Path(args[args.index("--params-file") + 1]))
            self.assertEqual(json.loads(files[-1].read_bytes()), {"中文": 2**64-1})
            child = original([sys.executable, "-c", "import time; time.sleep(30)"], **kwargs)
            children.append(child)
            return child

        start = time.monotonic()
        with patch("decker.client.subprocess.Popen", side_effect=launch):
            with self.assertRaises(CallTimeout) as caught:
                Client("unit").call("scene.new", {"中文": 2**64-1}, timeout=0.2)
        self.assertEqual(caught.exception.execution, "unknown")
        self.assertIsNone(caught.exception.ticket)
        self.assertLess(time.monotonic() - start, 5)
        self.assertIsNotNone(children[0].poll())
        self.assertFalse(files[0].exists())
        self.assertTrue(children[0].stdout.closed)

    def test_batch_has_one_deadline_and_partial_results(self):
        client = Client("unit")
        calls = []

        def exchange(method, params, deadline):
            calls.append(deadline)
            if len(calls) == 2:
                raise CallTimeout("timeout", status="timeout", execution="unknown")
            return Reply(response(1))

        with patch.object(client, "_exchange", side_effect=exchange):
            with self.assertRaises(BatchError) as caught:
                client.batch([Command("one"), Command("two"), Command("three")])
        self.assertEqual(calls[0], calls[1])
        self.assertEqual(caught.exception.failed_index, 1)
        self.assertEqual(len(caught.exception.completed), 1)
        self.assertEqual(caught.exception.cause.execution, "unknown")

    def test_job_wait_terminal_failure_timeout_and_no_cancel(self):
        client = Client("unit")
        for state in ("succeeded", "failed", "cancelled"):
            values = [Reply(response({"job": job("running"), "timed_out": True})),
                      Reply(response({"job": job(state), "timed_out": False}))]
            with patch.object(client, "call", side_effect=values) as call:
                if state == "succeeded":
                    self.assertEqual(client.wait_job(JOB)["state"], state)
                else:
                    with self.assertRaises(JobError) as caught:
                        client.wait_job(JOB)
                    self.assertEqual(caught.exception.job["state"], state)
                self.assertTrue(all(c.args[0] == "jobs.wait" for c in call.call_args_list))
        running = Reply(response({"job": job("running"), "timed_out": True}))
        with patch.object(client, "call", return_value=running) as call:
            with self.assertRaises(WaitTimeout) as caught:
                client.wait_job(JOB, timeout=0.03)
            self.assertEqual(caught.exception.last_job["state"], "running")
            self.assertEqual(caught.exception.job_id, JOB)
            self.assertLess(call.call_count, 10)
        with patch.object(client, "call", side_effect=CallTimeout("lost", status="timeout", execution="unknown")):
            with self.assertRaises(WaitTimeout) as caught:
                client.wait_job(JOB)
            self.assertIsInstance(caught.exception.__cause__, CallTimeout)

    def test_capture_preserves_submission_on_timeout_and_checks_identity(self):
        client = Client("unit")
        submitted = {"job_id": JOB, "document_id": SESSION, "scene_id": TASK, "revision": 1,
                     "frame": 2, "width": 2, "height": 2, "output": "test.ppm"}
        result = dict(submitted, kind="capture", draw_count=1)
        with patch.object(client, "_exchange", return_value=Reply(response(submitted))):
            with patch.object(client, "_wait_job", side_effect=WaitTimeout(JOB, None)):
                with self.assertRaises(WaitTimeout) as caught:
                    client.capture({})
                self.assertEqual(caught.exception.submission.value["job_id"], JOB)
            with patch.object(client, "_wait_job", return_value=job(result=result)):
                self.assertEqual(client.capture({}).job["result"]["frame"], 2)
            result["revision"] = 3
            with patch.object(client, "_wait_job", return_value=job(result=result)):
                with self.assertRaises(TransportError):
                    client.capture({})

    def test_malformed_job_result_is_not_a_completed_job(self):
        client = Client("unit")
        malformed = job()
        del malformed["result"]
        with patch.object(client, "call", return_value=Reply(response({"job": malformed, "timed_out": False}))):
            with self.assertRaises(TransportError):
                client.wait_job(JOB)

    def test_artifact_rejects_escape_and_corrupt_payload(self):
        result = {"output": "test.ppm", "width": 2, "height": 2}
        capture = Capture(Reply(response()), job(result=result))
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            data = b"P6\n2 2\n255\n" + b"\0\1\2" * 4
            (root / "test.ppm").write_bytes(data)
            artifact = capture.collect(root)
            self.assertEqual(artifact.size_bytes, len(data))
            self.assertEqual(len(artifact.sha256), 64)
            self.assertEqual(artifact.path, (root / "test.ppm").resolve())
            for bad in (data[:-1], data + b"x", data.replace(b"P6", b"P3")):
                (root / "test.ppm").write_bytes(bad)
                with self.assertRaises(ArtifactError):
                    capture.collect(root)
            for name in ("../test.ppm", "C:/test.ppm", "/test.ppm", "a\\test.ppm", "a//b.ppm"):
                result["output"] = name
                with self.assertRaises(ArtifactError):
                    capture.collect(root)


if __name__ == "__main__":
    unittest.main()
