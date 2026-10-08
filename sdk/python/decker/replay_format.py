"""Version 1 recording codec and bounded project fingerprints (standard library only)."""

import hashlib
import json
import math
import os
from pathlib import Path
import tempfile

from .client import _command, _finite_float, _reject_constant, _unique_object
from .models import ClientError, _uuid

MAX_RECORD = 16 * 1024 * 1024
MAX_INPUT = 512 * 1024 * 1024
EDITS = frozenset(("entity.create", "entity.delete", "entity.set_name", "entity.set_transform",
                   "entity.set_parent", "entity.set_assets"))
METHODS = EDITS | {"entity.get", "scene.query", "scene.transaction", "scene.save", "project.save",
                   "history.status", "history.undo", "history.redo"}


class ReplayError(ClientError):
    def __init__(self, message, *, phase="validate", step=None, completed=0):
        super().__init__(message)
        self.phase, self.step, self.completed = phase, step, completed


def canonical(value):
    return json.dumps(value, ensure_ascii=False, allow_nan=False, sort_keys=True,
                      separators=(",", ":")).encode("utf-8")


def digest(value):
    return hashlib.sha256(canonical(value)).hexdigest()


def relative(name):
    if (not isinstance(name, str) or not name or "\\" in name or ":" in name or "\0" in name
            or any(p in ("", ".", "..") for p in name.split("/"))):
        raise ReplayError("Expected a normalized project-relative path")
    if name.split("/")[0].casefold() == ".decker":
        raise ReplayError("Derived .decker files are not recording inputs/outputs")
    return name


def path_in(root, name):
    path = root / relative(name)
    if not path.resolve().is_relative_to(root):
        raise ReplayError("Project path escapes the root")
    return path


def _reparse(path):
    return path.is_symlink() or bool(getattr(path.lstat(), "st_file_attributes", 0) & 0x400)


def file_stamp(path):
    if _reparse(path) or not path.is_file():
        raise ReplayError("Input/output must be a regular file without reparse points")
    size, sha = 0, hashlib.sha256()
    with path.open("rb") as stream:
        while chunk := stream.read(1024 * 1024):
            size += len(chunk)
            if size > MAX_INPUT:
                raise ReplayError("Input file exceeds 512 MiB")
            sha.update(chunk)
    return {"size": size, "sha256": sha.hexdigest()}


def inventory(root):
    if (root / ".decker/asset-operations/pending.json").exists():
        raise ReplayError("Recover pending asset operations before recording/replay")
    files, total = {}, 0

    def visit(directory):
        nonlocal total
        for path in sorted(directory.iterdir()):
            # Only the root engine cache is derived. Never follow links, even to cache.
            if _reparse(path):
                raise ReplayError(f"Reparse point in project: {path.name}")
            if directory == root and path.name.casefold() == ".decker":
                continue
            if path.is_dir():
                visit(path)
            elif path.is_file():
                name = relative(path.relative_to(root).as_posix())
                stamp = file_stamp(path)
                total += stamp["size"]
                files[name] = stamp
                if len(files) > 4096 or total > MAX_INPUT:
                    raise ReplayError("Project inputs exceed 4096 files / 512 MiB")
            else:
                raise ReplayError("Project contains a non-regular file")

    visit(root)
    return files


def read_json(path):
    with Path(path).open("rb") as stream:
        data = stream.read(MAX_RECORD + 1)
    if len(data) > MAX_RECORD:
        raise ReplayError("JSON file exceeds 16 MiB")
    return json.loads(data.decode("utf-8"), object_pairs_hook=_unique_object,
                      parse_constant=_reject_constant, parse_float=_finite_float)


def _keys(value, expected):
    if not isinstance(value, dict) or set(value) != set(expected.split()):
        raise ReplayError("Invalid or unknown recording fields")


def _sha(value):
    return isinstance(value, str) and len(value) == 64 and all(c in "0123456789abcdef" for c in value)


def _stamps(values):
    if not isinstance(values, dict) or len(values) > 4096:
        raise ReplayError("Invalid file inventory")
    aliases = set()
    total = 0
    for name, stamp in values.items():
        relative(name)
        if name.casefold() in aliases:
            raise ReplayError("Case-aliased project paths")
        aliases.add(name.casefold())
        _keys(stamp, "size sha256")
        if type(stamp["size"]) is not int or not 0 <= stamp["size"] <= MAX_INPUT or not _sha(stamp["sha256"]):
            raise ReplayError("Invalid file fingerprint")
        total += stamp["size"]
    if total > MAX_INPUT:
        raise ReplayError("Project fingerprint exceeds 512 MiB")


def validate_command(method, params, header, kind="call"):
    _command(method, params)
    if kind == "capture":
        if method != "render.capture" or not relative(params.get("output")).lower().endswith(".ppm"):
            raise ReplayError("Invalid capture step")
    elif kind != "call" or method not in METHODS:
        raise ReplayError(f"Unsupported recorded method: {method}")
    if method == "project.save" and params.get("manifest") != header["manifest"]:
        raise ReplayError("Recording project.save must target the original manifest")
    if method == "scene.transaction":
        commands = params.get("commands")
        if not isinstance(commands, list) or not 1 <= len(commands) <= 128:
            raise ReplayError("Invalid transaction commands")
        for command in commands:
            _keys(command, "method params")
            if command["method"] not in EDITS:
                raise ReplayError("Only entity memory edits are recordable in transactions")
            _command(command["method"], command["params"])
            if command["method"] == "entity.create" and "id" not in command["params"]:
                raise ReplayError("Recorded entity creation requires a materialized ID")
    if method == "entity.create" and "id" not in params:
        raise ReplayError("Recorded entity creation requires a materialized ID")


def _logical_snapshot(snapshot):
    _keys(snapshot, "state entities")
    state, entities = snapshot["state"], snapshot["entities"]
    _keys(state, "scene_id revision dirty entity_count")
    if (not _uuid(state["scene_id"]) or type(state["revision"]) is not int or state["revision"] < 0
            or type(state["dirty"]) is not bool or type(state["entity_count"]) is not int
            or not isinstance(entities, list) or not 0 <= len(entities) <= 10000
            or state["entity_count"] != len(entities)):
        raise ReplayError("Invalid logical scene state")
    ids = []
    for entity in entities:
        _keys(entity, "id name transform parent assets")
        if (not _uuid(entity["id"]) or not isinstance(entity["name"], str)
                or (entity["parent"] is not None and not _uuid(entity["parent"]))
                or not isinstance(entity["assets"], list) or len(entity["assets"]) > 64):
            raise ReplayError("Invalid logical entity")
        ids.append(entity["id"])
        _keys(entity["transform"], "translation rotation scale")
        for field, size in (("translation", 3), ("rotation", 4), ("scale", 3)):
            numbers = entity["transform"][field]
            if not isinstance(numbers, list) or len(numbers) != size or any(type(n) not in (int, float) for n in numbers):
                raise ReplayError("Invalid logical transform")
        for asset in entity["assets"]:
            _keys(asset, "id kind")
            if not _uuid(asset["id"]) or asset["kind"] not in ("mesh", "material", "texture"):
                raise ReplayError("Invalid logical asset reference")
    if ids != sorted(set(ids)):
        raise ReplayError("Logical entities must be unique and sorted")
    id_set = set(ids)
    if any(e["parent"] is not None and (e["parent"] not in id_set or e["parent"] == e["id"]) for e in entities):
        raise ReplayError("Logical parent reference is invalid")


def validate(record):
    try:
        pending, nodes = [(record, 0)], 0
        while pending:
            item, depth = pending.pop()
            nodes += 1
            if depth > 64 or nodes > 1000000:
                raise ReplayError("Recording structural limit exceeded")
            if type(item) is int and not -(2**63) <= item < 2**64:
                raise ReplayError("Recording integer exceeds int64/uint64")
            if type(item) is float and not math.isfinite(item):
                raise ReplayError("Recording contains a non-finite number")
            if isinstance(item, dict):
                pending.extend((child, depth + 1) for child in item.values())
            elif isinstance(item, list):
                pending.extend((child, depth + 1) for child in item)
        _keys(record, "format version header initial steps final sha256")
        if record["format"] != "DeckerRecording" or type(record["version"]) is not int or record["version"] != 1:
            raise ReplayError("Unsupported recording version")
        payload = {key: value for key, value in record.items() if key != "sha256"}
        encoded = canonical(record)
        if len(encoded) > MAX_RECORD or not _sha(record["sha256"]) or digest(payload) != record["sha256"]:
            raise ReplayError("Recording size or checksum mismatch")
        header = record["header"]
        _keys(header, "sdk_version protocol capabilities commands_sha256 engine_build seed context environment root manifest scene inputs document_id")
        if type(header["sdk_version"]) is not int or header["sdk_version"] != 1 or type(header["protocol"]) is not int or header["protocol"] != 1:
            raise ReplayError("Unsupported SDK/IPC recording protocol")
        if (not isinstance(header["engine_build"], str) or not header["engine_build"]
                or type(header["seed"]) is not int or not 0 <= header["seed"] < 2**64
                or not _sha(header["commands_sha256"]) or not _uuid(header["document_id"])
                or not isinstance(header["context"], dict) or not isinstance(header["capabilities"], dict)
                or not isinstance(header["environment"], dict) or not isinstance(header["root"], str)):
            raise ReplayError("Invalid recording context")
        _command("context", header["context"])
        _stamps(header["inputs"])
        if (relative(header["manifest"]) not in header["inputs"] or relative(header["scene"]) not in header["inputs"]
                or header["manifest"] == header["scene"]):
            raise ReplayError("Missing initial manifest or scene input")
        for snapshot in (record["initial"], record["final"]):
            _logical_snapshot(snapshot)
        if not isinstance(record["steps"], list) or len(record["steps"]) > 1024:
            raise ReplayError("Invalid step count")
        outputs = {name.casefold() for name in header["inputs"]}
        for step in record["steps"]:
            _keys(step, "kind method params outcome after writes")
            validate_command(step["method"], step["params"], header, step["kind"])
            if not _sha(step["after"]):
                raise ReplayError("Invalid state fingerprint")
            outcome = step["outcome"]
            if outcome.get("kind") == "success":
                _keys(outcome, "kind value")
            elif outcome.get("kind") == "rpc_error" and step["kind"] == "call":
                _keys(outcome, "kind code engine_name message")
                if type(outcome["code"]) is not int or not isinstance(outcome["message"], str):
                    raise ReplayError("Invalid recorded RPC error")
            else:
                raise ReplayError("Incomplete or unsupported step outcome")
            _stamps(step["writes"])
            expected = set()
            if outcome["kind"] == "success":
                if step["kind"] == "capture":
                    output = relative(step["params"]["output"])
                    if output.casefold() in outputs:
                        raise ReplayError("Capture output overlaps an input or previous output")
                    outputs.add(output.casefold())
                    expected.add(output)
                elif step["method"] in ("scene.save", "project.save"):
                    expected.add(header["scene" if step["method"] == "scene.save" else "manifest"])
            if set(step["writes"]) != expected:
                raise ReplayError("Recorded output set does not match command")
        expected_final = record["steps"][-1]["after"] if record["steps"] else digest(record["initial"])
        if digest(record["final"]) != expected_final:
            raise ReplayError("Final snapshot does not match last step")
        return record
    except ReplayError:
        raise
    except (ValueError, TypeError, KeyError, AttributeError, RecursionError) as error:
        raise ReplayError(f"Malformed recording: {error}") from error


def load_recording(path):
    try:
        return validate(read_json(path))
    except ReplayError:
        raise
    except (OSError, ValueError, RecursionError) as error:
        raise ReplayError(f"Cannot load recording: {error}") from error


def atomic_record(path, record):
    validate(record)
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(dir=path.parent, prefix=".dk-record-", suffix=".tmp", delete=False) as stream:
            temporary = Path(stream.name)
            stream.write(canonical(record))
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)
