"""Record and verify commands against an independent copy of an initial project."""

import argparse
from dataclasses import dataclass
import json
from pathlib import Path
import platform
import random
import time
from uuid import UUID

from .client import Client, Command, _command, _seconds
from .models import RpcError
from .replay_format import (
    MAX_RECORD, METHODS, ReplayError, atomic_record, canonical, digest, file_stamp,
    inventory, load_recording, path_in, read_json, relative, validate, validate_command,
)


def _left(deadline):
    left = deadline - time.monotonic()
    if left < 0.001:
        raise ReplayError("Recording/replay deadline reached", phase="timeout")
    return left


def _call(client, method, params, deadline):
    return client.call(method, params, timeout=min(client.timeout, _left(deadline)))


def _contract(client, deadline):
    hello = client.hello(timeout=min(client.timeout, _left(deadline)))
    caps = _call(client, "runtime.capabilities", {}, deadline).value
    names = METHODS | {"scene.load"}
    if caps.get("render_capture"):
        names = names | {"render.capture", "jobs.wait"}
    descriptions = [_call(client, "commands.describe", {"name": name}, deadline).value for name in sorted(names)]
    return {"protocol": hello["protocol"], "capabilities": caps, "commands_sha256": digest(descriptions)}


def _normalize(value, document):
    if isinstance(value, dict):
        return {key: ("$document" if key == "document_id" and child == document else _normalize(child, document))
                for key, child in value.items()}
    if isinstance(value, list):
        return [_normalize(child, document) for child in value]
    return value


def _snapshot(client, deadline):
    state, entities, offset = None, [], 0
    while True:
        page = _call(client, "scene.query", {"offset": offset, "limit": 256}, deadline).value
        if state is None:
            state = page["state"]
        if (page["state"] != state or page["offset"] != offset or not isinstance(page["entities"], list)
                or len(page["entities"]) > 256 or type(page["has_more"]) is not bool):
            raise ReplayError("Scene changed during snapshot pagination", phase="snapshot")
        for entity in page["entities"]:
            entities.append({key: entity[key] for key in ("id", "name", "transform", "parent", "assets")})
        offset += len(page["entities"])
        if offset > 10000 or (page["has_more"] and not page["entities"]):
            raise ReplayError("Invalid scene pagination", phase="snapshot")
        if not page["has_more"]:
            break
    if len(entities) != state["entity_count"] or len({entity["id"] for entity in entities}) != len(entities):
        raise ReplayError("Scene snapshot has missing/duplicate entities", phase="snapshot")
    logical = {"state": {key: state[key] for key in ("scene_id", "revision", "dirty", "entity_count")},
               "entities": sorted(entities, key=lambda entity: entity["id"])}
    return state, logical


def _check_state(client, deadline, expected, document):
    state, logical = _snapshot(client, deadline)
    if state["document_id"] != document or digest(logical) != expected:
        raise ReplayError("Logical scene state diverged", phase="state")
    return state, logical


def _check_files(root, expected):
    if inventory(root) != expected:
        raise ReplayError("Project input/output fingerprints diverged", phase="inputs")


def _bootstrap(client, root, manifest, deadline):
    try:
        _call(client, "scene.query", {}, deadline)
    except RpcError as error:
        if error.code != -32002:
            raise
    else:
        raise ReplayError("Recording/replay requires a host with no active scene", phase="bootstrap")
    _call(client, "scene.load", {"manifest": manifest}, deadline)
    return _snapshot(client, deadline)


def _error_outcome(error):
    data = error.data if isinstance(error.data, dict) else {}
    return {"kind": "rpc_error", "code": error.code, "engine_name": data.get("engine_name"), "message": str(error)}


def _outcome_equal(actual, expected):
    if actual["kind"] == expected["kind"] == "rpc_error":
        return all(actual[key] == expected[key] for key in ("code", "engine_name"))
    return canonical(actual) == canonical(expected)


def _execute(client, root, header, kind, method, params, deadline, document):
    writes, error, returned = {}, None, None
    if kind == "capture":
        target = path_in(root, params["output"])
        if target.exists():
            raise ReplayError("Capture output already exists", phase="inputs")
        returned = client.capture(params, timeout=_left(deadline))
        artifact = returned.collect(root)
        writes[params["output"]] = {"size": artifact.size_bytes, "sha256": artifact.sha256}
        outcome = {"kind": "success", "value": _normalize(returned.job["result"], document)}
    else:
        try:
            returned = _call(client, method, params, deadline)
            outcome = {"kind": "success", "value": _normalize(returned.value, document)}
            if method in ("scene.save", "project.save"):
                name = header["scene" if method == "scene.save" else "manifest"]
                writes[name] = file_stamp(path_in(root, name))
        except RpcError as failure:
            error = failure
            outcome = _error_outcome(failure)
    return returned, outcome, writes, error


class Recorder:
    """Use start() on a fresh host. Only successful save() publishes a complete record."""

    @classmethod
    def start(cls, client, project_root, *, manifest="project.json", seed, engine_build, context=None, timeout=60):
        deadline = time.monotonic() + _seconds(timeout, 86400)
        if type(seed) is not int or not 0 <= seed < 2**64 or not isinstance(engine_build, str) or not engine_build:
            raise ValueError("seed must be uint64 and engine_build must identify the host build")
        context = json.loads(_command("context", {} if context is None else context))
        root = Path(project_root).resolve(strict=True)
        files = inventory(root)
        project = read_json(path_in(root, manifest))
        scene = relative(project["scene"])
        if (project.get("format") != "DeckerProject" or type(project.get("version")) is not int
                or project["version"] != 1 or manifest not in files or scene not in files or scene == manifest):
            raise ReplayError("Expected existing DeckerProject v1 and Scene inputs")
        contract = _contract(client, deadline)
        state, initial = _bootstrap(client, root, manifest, deadline)
        self = cls()
        self.client, self.root, self.rng = client, root, random.Random(seed)
        self.header = dict(contract, sdk_version=1, engine_build=engine_build, seed=seed, context=context,
                           environment={"python": platform.python_version(), "os": platform.platform()},
                           root=str(root), manifest=manifest, scene=scene, inputs=files, document_id=state["document_id"])
        self.initial, self.final, self.steps = initial, initial, []
        self._files, self._broken, self._closed = dict(files), False, False
        _check_files(root, files)
        return self

    def _active(self):
        if self._broken or self._closed:
            raise ReplayError("Recorder is invalid or already saved", phase="record")
        if len(self.steps) >= 1024:
            raise ReplayError("Recording step capacity exhausted", phase="record")

    def _materialize(self, method, params):
        params = json.loads(_command(method, {} if params is None else params))

        def create(name, value):
            if name == "entity.create" and "id" not in value:
                value["id"] = str(UUID(int=self.rng.getrandbits(128), version=4))

        create(method, params)
        if method == "scene.transaction" and isinstance(params.get("commands"), list):
            for command in params["commands"]:
                if isinstance(command, dict) and isinstance(command.get("params"), dict):
                    create(command.get("method"), command["params"])
        return params

    def _record(self, method, params, kind, timeout):
        self._active()
        deadline = time.monotonic() + _seconds(timeout, 86400)
        params = self._materialize(method, params)
        validate_command(method, params, self.header, kind)
        if kind == "capture" and params["output"].casefold() in {name.casefold() for name in self._files}:
            raise ReplayError("Capture output overlaps an input or previous output")
        try:
            _check_files(self.root, self._files)
            _check_state(self.client, deadline, digest(self.final), self.header["document_id"])
            returned, outcome, writes, error = _execute(self.client, self.root, self.header, kind, method, params,
                                                       deadline, self.header["document_id"])
            state, logical = _snapshot(self.client, deadline)
            if state["document_id"] != self.header["document_id"]:
                raise ReplayError("Scene was replaced during recording")
            step = {"kind": kind, "method": method, "params": params, "outcome": outcome,
                    "after": digest(logical), "writes": writes}
            self.steps.append(step)
            self.final = logical
            self._files.update(writes)
            _check_files(self.root, self._files)
            if len(canonical(self._recording())) > MAX_RECORD:
                raise ReplayError("Recording capacity exhausted after execution")
        except BaseException:
            self._broken = True
            raise
        if error is not None:
            raise error
        return returned

    def call(self, method, params=None, *, timeout=30):
        return self._record(method, params, "call", timeout)

    def transaction(self, guard, commands, *, timeout=30):
        commands = Client._commands(commands)
        return self.call("scene.transaction", {"guard": guard, "commands": [
            {"method": command.method, "params": command.params} for command in commands]}, timeout=timeout)

    def capture(self, params, *, timeout=60):
        return self._record("render.capture", params, "capture", timeout)

    def _recording(self):
        value = {"format": "DeckerRecording", "version": 1, "header": self.header,
                 "initial": self.initial, "steps": self.steps, "final": self.final}
        return dict(value, sha256=digest(value))

    def save(self, path, *, timeout=60):
        if self._broken or self._closed:
            raise ReplayError("Recorder is invalid or already saved", phase="save")
        path = Path(path).resolve()
        if path.is_relative_to(self.root):
            raise ReplayError("Save recording outside the project input tree", phase="save")
        deadline = time.monotonic() + _seconds(timeout, 86400)
        try:
            _check_files(self.root, self._files)
            _check_state(self.client, deadline, digest(self.final), self.header["document_id"])
        except BaseException:
            self._broken = True
            raise
        record = self._recording()
        atomic_record(path, record)
        self._closed = True
        return path


@dataclass(frozen=True)
class ReplayReport:
    completed: int
    final_sha256: str
    artifacts: tuple[dict, ...]


def replay(client, recording, project_root, *, engine_build, timeout=300):
    deadline = time.monotonic() + _seconds(timeout, 86400)
    completed, index, phase = 0, None, "preflight"
    try:
        # Own a validated immutable-by-convention copy before any external action.
        record = load_recording(recording) if isinstance(recording, (str, Path)) else validate(json.loads(canonical(recording)))
        header = record["header"]
        root = Path(project_root).resolve(strict=True)
        if engine_build != header["engine_build"]:
            raise ReplayError("Engine build identifier mismatch")
        _check_files(root, header["inputs"])
        contract = _contract(client, deadline)
        if any(contract[key] != header[key] for key in contract):
            raise ReplayError("Host protocol/capabilities/command schemas differ")
        phase = "bootstrap"
        state, initial = _bootstrap(client, root, header["manifest"], deadline)
        if digest(initial) != digest(record["initial"]):
            raise ReplayError("Initial logical state differs")
        document = state["document_id"]
        expected, files, artifacts = digest(initial), dict(header["inputs"]), []
        for index, step in enumerate(record["steps"]):
            phase = "before"
            _check_files(root, files)
            _check_state(client, deadline, expected, document)
            params = json.loads(canonical(step["params"]))
            # Only this known session identity changes; revisions and invalid guards stay literal.
            guard = params.get("guard")
            if isinstance(guard, dict) and guard.get("document_id") == header["document_id"]:
                guard["document_id"] = document
            phase = "execute"
            _, outcome, writes, _ = _execute(client, root, header, step["kind"], step["method"], params, deadline, document)
            phase = "verify"
            if not _outcome_equal(outcome, step["outcome"]):
                raise ReplayError("Command result differs from recording")
            if step["kind"] != "capture" and writes != step["writes"]:
                raise ReplayError("Saved file differs from recording")
            if step["kind"] == "capture":
                for name, stamp in writes.items():
                    artifacts.append({"output": name, **stamp, "recorded_sha256": step["writes"][name]["sha256"]})
            files.update(writes)
            _check_files(root, files)
            _check_state(client, deadline, step["after"], document)
            expected = step["after"]
            completed += 1
        phase = "final"
        _check_state(client, deadline, digest(record["final"]), document)
        _check_files(root, files)
        _left(deadline)
        return ReplayReport(completed, expected, tuple(artifacts))
    except Exception as error:
        raise ReplayError(f"Replay stopped during {phase}: {error}", phase=phase, step=index, completed=completed) from error


def main():
    parser = argparse.ArgumentParser(description="Verify and replay a DeckerRecording v1 on a fresh host")
    parser.add_argument("recording")
    parser.add_argument("--pipe", required=True)
    parser.add_argument("--ctl", default="dk-ctl")
    parser.add_argument("--project-root", required=True)
    parser.add_argument("--engine-build", required=True)
    parser.add_argument("--timeout", type=float, default=300)
    args = parser.parse_args()
    try:
        report = replay(Client(args.pipe, args.ctl), args.recording, args.project_root,
                        engine_build=args.engine_build, timeout=args.timeout)
        print(json.dumps({"completed": report.completed, "final_sha256": report.final_sha256,
                          "artifacts": report.artifacts}))
    except Exception as error:
        print(json.dumps({"error": str(error), "phase": getattr(error, "phase", "input"),
                          "step": getattr(error, "step", None), "completed": getattr(error, "completed", 0)}))
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
