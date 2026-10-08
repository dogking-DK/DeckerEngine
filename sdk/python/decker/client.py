"""Standard-library dk-ctl adapter. No retries, guard refresh, or implicit shutdown."""

from dataclasses import dataclass, field
import json
import math
import os
from pathlib import Path
import re
import subprocess
import tempfile
import time
from typing import Any, Sequence

from .models import (
    BatchError, CallTimeout, Capture, ClientError, JobError, Reply, RpcError,
    Ticket, TransportError, WaitTimeout, _uuid,
)


def _seconds(value: float, maximum: float = 60) -> float:
    if type(value) not in (int, float) or not 0.001 <= value <= maximum:
        raise ValueError(f"timeout must be finite, in 0.001..{maximum} seconds")
    return float(value)


def _remaining(deadline: float) -> float:
    remaining = deadline - time.monotonic()
    if remaining < 0.001:
        raise CallTimeout("Deadline reached before launching dk-ctl", status="timeout", execution="not_sent")
    return remaining


def _params(value: dict) -> bytes:
    if type(value) is not dict:
        raise ValueError("params must be a JSON object")
    nodes = 0
    string_bytes = 0

    def charge_string(item):
        nonlocal string_bytes
        if len(item) > 1024 * 1024:
            raise ValueError("params exceed 1 MiB")
        string_bytes += len(item.encode("utf-8"))
        if string_bytes > 1024 * 1024:
            raise ValueError("params exceed 1 MiB")

    def check(item, depth):
        nonlocal nodes
        nodes += 1
        if depth > 64 or nodes > 200000:
            raise ValueError("params exceed depth/node limits")
        kind = type(item)
        if item is None or kind is bool:
            return
        if kind is str:
            charge_string(item)
            return
        if kind is int and -(2**63) <= item < 2**64:
            return
        if kind is float and math.isfinite(item):
            return
        if kind is list:
            for child in item:
                check(child, depth + 1)
            return
        if kind is dict:
            for key, child in item.items():
                if type(key) is not str:
                    raise ValueError("JSON object keys must be strings")
                charge_string(key)
                check(child, depth + 1)
            return
        raise ValueError("params require JSON types, finite floats and int64/uint64 integers")

    check(value, 0)
    try:
        encoded = json.dumps(value, ensure_ascii=False, allow_nan=False, separators=(",", ":")).encode("utf-8")
    except (ValueError, UnicodeError) as error:
        raise ValueError("params must be valid UTF-8 JSON") from error
    if len(encoded) > 1024 * 1024:
        raise ValueError("params exceed 1 MiB")
    return encoded


def _command(method: str, params: dict) -> bytes:
    if not isinstance(method, str) or "\0" in method or not 1 <= len(method.encode("utf-8")) <= 256:
        raise ValueError("method must contain 1..256 UTF-8 bytes without NUL")
    return _params(params)


def _unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError("Duplicate JSON key")
        result[key] = value
    return result


def _reject_constant(value):
    raise ValueError(f"Non-finite JSON constant: {value}")


def _finite_float(value):
    number = float(value)
    if not math.isfinite(number):
        raise ValueError("Non-finite JSON number")
    return number


def _decode(output: bytes, stderr: bytes, code: int, hello: bool, retry: Ticket | None):
    diagnostic = stderr.decode("utf-8", errors="replace")
    unknown = "not_sent" if hello else "unknown"
    raw = None
    ticket = retry
    try:
        if len(output) > 17 * 1024 * 1024:
            raise ValueError("dk-ctl output exceeds response limit")
        raw = json.loads(output.decode("utf-8"), object_pairs_hook=_unique_object,
                         parse_constant=_reject_constant, parse_float=_finite_float)
        if not isinstance(raw, dict) or raw.get("execution") not in ("not_sent", "unknown", "received"):
            raise ValueError("Invalid dk-ctl execution state")
        if "session" in raw or "request_id" in raw:
            ticket = Ticket(raw.get("session"), raw.get("request_id"))
        if retry is not None and ticket != retry:
            raise ValueError("Retry ticket changed")
        status = raw.get("status")
        if not isinstance(status, str):
            raise ValueError("Missing transport status")
        if status != "ok":
            if code not in (2, 3) or raw["execution"] == "received":
                raise ValueError("Inconsistent transport failure")
            cls = CallTimeout if status == "timeout" else TransportError
            raise cls(str(raw.get("message", status)), status=status, execution=raw["execution"],
                      ticket=ticket, raw=raw, stderr=diagnostic)
        if hello:
            value = raw.get("hello")
            if (code != 0 or raw["execution"] != "not_sent" or not isinstance(value, dict)
                    or type(value.get("protocol")) is not int or value["protocol"] != 1
                    or not _uuid(value.get("session")) or not isinstance(value.get("limits"), dict)):
                raise ValueError("Invalid hello response")
            return value
        response = raw.get("response")
        if (ticket is None or "session" not in raw or "request_id" not in raw
                or raw["execution"] != "received" or not isinstance(response, dict)
                or response.get("jsonrpc") != "2.0" or type(response.get("id")) is not int
                or response["id"] != ticket.request_id or len(response) != 3
                or ("result" in response) == ("error" in response)):
            raise ValueError("Invalid JSON-RPC response")
        if "error" in response:
            error = response["error"]
            if (code != 1 or not isinstance(error, dict) or type(error.get("code")) is not int
                    or not isinstance(error.get("message"), str)):
                raise ValueError("Invalid JSON-RPC error")
            raise RpcError(raw)
        result = response["result"]
        if (code != 0 or not isinstance(result, dict) or result.get("status") != "succeeded"
                or not _uuid(result.get("task_id")) or "value" not in result):
            raise ValueError("Invalid Task result")
        return Reply(raw)
    except (ValueError, UnicodeError, RecursionError) as error:
        raise TransportError(f"Invalid dk-ctl output: {error}", status="invalid_response",
                             execution=unknown, ticket=ticket, raw=raw, stderr=diagnostic) from error


@dataclass(frozen=True)
class Command:
    method: str
    params: dict = field(default_factory=dict)


class Client:
    def __init__(self, endpoint: str, executable: str | os.PathLike = "dk-ctl", *, timeout: float = 5):
        if not isinstance(endpoint, str) or re.fullmatch(r"[A-Za-z0-9_.-]{1,64}", endpoint) is None:
            raise ValueError("endpoint must be 1..64 ASCII letters, digits, dots, hyphens or underscores")
        self.endpoint, self.executable, self.timeout = endpoint, os.fspath(executable), _seconds(timeout)

    def _exchange(self, method: str | None, encoded: bytes, deadline: float, retry: Ticket | None = None):
        if retry is not None and not isinstance(retry, Ticket):
            raise ValueError("retry must be a Ticket")
        execution = "not_sent"
        try:
            with tempfile.TemporaryDirectory(prefix="decker-") as directory:
                args = [self.executable, "--pipe", self.endpoint]
                if method is None:
                    args += ["--hello"]
                else:
                    path = Path(directory) / "params.json"
                    path.write_bytes(encoded)
                    args += ["--method", method, "--params-file", str(path)]
                if retry is not None:
                    args += ["--session", retry.session, "--request-id", str(retry.request_id)]
                args += ["--timeout-ms", str(min(60000, int(_remaining(deadline) * 1000)))]
                try:
                    process = subprocess.Popen(args, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                               stderr=subprocess.PIPE, shell=False,
                                               creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0)
                except OSError as error:
                    raise TransportError(str(error), status="launch_error", execution="not_sent", ticket=retry) from error
                execution = "not_sent" if method is None else "unknown"
                try:
                    output, stderr = process.communicate(timeout=max(0, deadline - time.monotonic()))
                except subprocess.TimeoutExpired as error:
                    process.kill()
                    process.communicate()
                    raise CallTimeout("Python deadline reached; command may still execute", status="timeout",
                                      execution=execution, ticket=retry) from error
                finally:
                    # Includes KeyboardInterrupt and exceptional pipe failures. Only our child is owned.
                    if process.poll() is None:
                        process.kill()
                        process.communicate()
                    for stream in (process.stdout, process.stderr):
                        if stream is not None:
                            stream.close()
                return _decode(output, stderr, process.returncode, method is None, retry)
        except OSError as error:
            raise TransportError(str(error), status="local_io", execution=execution, ticket=retry) from error

    def hello(self, *, timeout: float | None = None) -> dict:
        return self._exchange(None, b"", time.monotonic() + _seconds(self.timeout if timeout is None else timeout))

    def call(self, method: str, params: dict | None = None, *, timeout: float | None = None,
             retry: Ticket | None = None) -> Reply:
        deadline = time.monotonic() + _seconds(self.timeout if timeout is None else timeout)
        return self._exchange(method, _command(method, {} if params is None else params), deadline, retry)

    @staticmethod
    def _commands(commands: Sequence[Command]) -> list[Command]:
        if not isinstance(commands, (list, tuple)) or not 1 <= len(commands) <= 128:
            raise ValueError("commands must be a list/tuple of 1..128 Command values")
        if any(not isinstance(command, Command) for command in commands):
            raise ValueError("Expected Command values")
        return list(commands)

    def batch(self, commands: Sequence[Command], *, timeout: float | None = None) -> list[Reply]:
        deadline = time.monotonic() + _seconds(self.timeout if timeout is None else timeout)
        commands = self._commands(commands)
        # Snapshot and validate all local inputs before the first external mutation.
        encoded = [_command(command.method, command.params) for command in commands]
        completed = []
        for index, (command, params) in enumerate(zip(commands, encoded)):
            try:
                completed.append(self._exchange(command.method, params, deadline))
            except ClientError as error:
                raise BatchError(completed, index, error) from error
        return completed

    def transaction(self, guard: dict, commands: Sequence[Command], *, timeout: float | None = None) -> Reply:
        commands = self._commands(commands)
        return self.call("scene.transaction", {"guard": guard, "commands": [
            {"method": command.method, "params": command.params} for command in commands
        ]}, timeout=timeout)

    def task(self, task_id: str, *, timeout: float | None = None) -> dict:
        """Tasks are synchronous terminal records. Use wait_job for asynchronous work."""
        return self.call("tasks.get", {"id": task_id}, timeout=timeout).value

    def wait_job(self, job_id: str, *, timeout: float = 30) -> dict:
        return self._wait_job(job_id, time.monotonic() + _seconds(timeout, 86400))

    def _wait_job(self, job_id: str, deadline: float) -> dict:
        if not _uuid(job_id):
            raise ValueError("job_id must be a non-nil UUID")
        last = None
        while True:
            remaining = deadline - time.monotonic()
            if remaining < 0.001:
                raise WaitTimeout(job_id, last)
            budget = min(self.timeout, remaining)
            # Leave half the call budget for client startup and transport.
            wait_ms = min(1000, max(0, int(budget * 500)))
            try:
                reply = self.call("jobs.wait", {"id": job_id, "timeout_ms": wait_ms}, timeout=budget)
            except CallTimeout as error:
                raise WaitTimeout(job_id, last) from error
            value = reply.value
            job = value.get("job") if isinstance(value, dict) else None
            if (not isinstance(job, dict) or job.get("id") != job_id
                    or job.get("state") not in ("queued", "running", "succeeded", "failed", "cancelled")
                    or type(value.get("timed_out")) is not bool
                    or type(job.get("cancel_requested")) is not bool
                    or "result" not in job or "error" not in job
                    or value["timed_out"] != (job["state"] in ("queued", "running"))):
                raise TransportError("Invalid jobs.wait result", status="invalid_response",
                                     execution="received", raw=reply.raw, ticket=reply.ticket)
            last = job
            if job["state"] == "succeeded":
                return job
            if job["state"] in ("failed", "cancelled"):
                raise JobError(job)
            # Avoid busy spinning if the server returned early or a zero wait was needed.
            time.sleep(min(0.01, max(0, deadline - time.monotonic())))

    def capture(self, params: dict, *, timeout: float = 60) -> Capture:
        deadline = time.monotonic() + _seconds(timeout, 86400)
        submission = self._exchange("render.capture", _command("render.capture", params),
                                    min(deadline, time.monotonic() + self.timeout))
        try:
            submitted = submission.value
            fields = ("document_id", "scene_id", "revision", "frame", "width", "height", "output")
            if (not isinstance(submitted, dict) or not _uuid(submitted.get("job_id"))
                    or any(key not in submitted for key in fields)
                    or not _uuid(submitted["document_id"]) or not _uuid(submitted["scene_id"])
                    or any(type(submitted[key]) is not int or not 0 <= submitted[key] < 2**64
                           for key in ("revision", "frame"))
                    or submitted["frame"] == 0
                    or any(type(submitted[key]) is not int or not 1 <= submitted[key] <= 2048
                           for key in ("width", "height"))
                    or not isinstance(submitted["output"], str) or not submitted["output"]):
                raise TransportError("Invalid capture submission", status="invalid_response",
                                     execution="received", raw=submission.raw, ticket=submission.ticket)
            job = self._wait_job(submitted["job_id"], deadline)
            result = job.get("result")
            if (not isinstance(result, dict) or result.get("kind") != "capture"
                    or any(type(result.get(key)) is not type(submitted[key]) or result.get(key) != submitted[key]
                           for key in fields)
                    or type(result.get("draw_count")) is not int or result["draw_count"] < 0):
                raise TransportError("Capture result does not match submitted snapshot", status="invalid_response",
                                     execution="received", raw=submission.raw, ticket=submission.ticket)
            return Capture(submission, job)
        except ClientError as error:
            error.submission = submission
            raise
