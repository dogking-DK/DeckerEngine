"""Owned response values and errors; no connection or engine lifetime is hidden."""

from dataclasses import dataclass
import hashlib
from pathlib import Path, PurePosixPath
from typing import Any
from uuid import UUID


def _uuid(value: Any) -> bool:
    try:
        return isinstance(value, str) and str(UUID(value)) == value.lower() and UUID(value).int != 0
    except (ValueError, AttributeError):
        return False


def guard(state: dict) -> dict:
    """Copy an explicit State's guard. Never query or refresh it implicitly."""
    if (not isinstance(state, dict) or not _uuid(state.get("document_id"))
            or type(state.get("revision")) is not int or not 0 <= state["revision"] < 2**64):
        raise ValueError("Expected a State with document_id and uint64 revision")
    return {"document_id": state["document_id"], "revision": state["revision"]}


@dataclass(frozen=True)
class Ticket:
    session: str
    request_id: int

    def __post_init__(self):
        if not _uuid(self.session) or type(self.request_id) is not int or not 0 < self.request_id < 2**63:
            raise ValueError("Ticket requires a non-nil session UUID and positive int64 request_id")


@dataclass(frozen=True)
class Reply:
    raw: dict

    @property
    def ticket(self) -> Ticket:
        return Ticket(self.raw["session"], self.raw["request_id"])

    @property
    def task_id(self) -> str:
        return self.raw["response"]["result"]["task_id"]

    @property
    def value(self) -> Any:
        return self.raw["response"]["result"]["value"]


class ClientError(Exception):
    """Base class for runtime client failures (invalid inputs use ValueError)."""

    submission: Reply | None = None


class TransportError(ClientError):
    def __init__(self, message: str, *, status: str, execution: str,
                 ticket: Ticket | None = None, raw: dict | None = None, stderr: str = ""):
        super().__init__(message)
        self.status, self.execution, self.ticket = status, execution, ticket
        self.raw, self.stderr = raw, stderr


class CallTimeout(TransportError):
    """The caller stopped waiting; this does not cancel or undo the engine command."""


class RpcError(ClientError):
    def __init__(self, raw: dict):
        error = raw["response"]["error"]
        super().__init__(error["message"])
        self.raw, self.code = raw, error["code"]
        self.data = error.get("data")
        self.ticket = Ticket(raw["session"], raw["request_id"])
        self.execution = "received"
        self.task_id = self.data.get("task_id") if isinstance(self.data, dict) else None


class BatchError(ClientError):
    def __init__(self, completed: list[Reply], failed_index: int, cause: ClientError):
        super().__init__(f"Batch stopped at index {failed_index}: {cause}")
        self.completed = tuple(completed)
        self.failed_index, self.cause = failed_index, cause


class JobError(ClientError):
    def __init__(self, job: dict):
        super().__init__(f"Job {job['id']} {job['state']}: {job.get('error')}")
        self.job = job


class WaitTimeout(ClientError):
    def __init__(self, job_id: str, last_job: dict | None):
        super().__init__(f"Deadline reached while waiting for job {job_id}; job was not cancelled")
        self.job_id, self.last_job = job_id, last_job


class ArtifactError(ClientError):
    pass


@dataclass(frozen=True)
class Artifact:
    path: Path
    width: int
    height: int
    size_bytes: int
    sha256: str


@dataclass(frozen=True)
class Capture:
    submission: Reply
    job: dict

    def collect(self, project_root: str | Path) -> Artifact:
        """Validate the current local PPM; use unique outputs to avoid later overwrites."""
        result = self.job["result"]
        name = result["output"]
        parts = name.split("/")
        if (not name or any(p in ("", ".", "..") for p in parts)
                or "\\" in name or ":" in name or "\0" in name or PurePosixPath(name).is_absolute()):
            raise ArtifactError("Capture output is not a normalized relative path")
        width, height = result["width"], result["height"]
        if any(type(n) is not int or not 1 <= n <= 2048 for n in (width, height)):
            raise ArtifactError("Invalid capture dimensions")
        header = f"P6\n{width} {height}\n255\n".encode("ascii")
        expected = len(header) + width * height * 3
        try:
            root = Path(project_root).resolve(strict=True)
            path = (root / name).resolve(strict=True)
            if not path.is_relative_to(root) or not path.is_file():
                raise ArtifactError("Capture output is outside the project or not a file")
            with path.open("rb") as stream:
                data = stream.read(expected + 1)
        except (OSError, ValueError, RuntimeError) as error:
            raise ArtifactError(f"Cannot read capture output: {error}") from error
        if len(data) != expected or not data.startswith(header):
            raise ArtifactError("Capture PPM header or payload does not match the completed job")
        return Artifact(path, width, height, len(data), hashlib.sha256(data).hexdigest())
