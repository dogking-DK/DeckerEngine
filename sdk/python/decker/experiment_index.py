"""Local experiment evidence index. Registration and verification never run simulations."""

import argparse
from contextlib import contextmanager
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import platform
import tempfile
from uuid import UUID, uuid4

from ._experiment_lock import snapshot_access

from .experiments import (ExperimentError, INDEX_LIMIT, canonical, clone_json, decode_json,
                          expand_plan, finite, integer, keys, normalize_config, read_json,
                          require, topology)

ARTIFACT_LIMIT = 16 * 1024 * 1024
METRICS = ("max_constraint_error", "rms_constraint_error", "max_relative_error", "max_speed",
           "kinetic_energy", "gravity_potential_energy", "compliant_energy", "min_height",
           "max_penetration", "max_pin_displacement")


def _now():
    return datetime.now(timezone.utc).isoformat()


def _uuid(value):
    try:
        require(type(value) is str and str(UUID(value)) == value and UUID(value).int != 0,
                "Expected canonical nonzero UUID")
    except ValueError as error:
        raise ExperimentError("Invalid UUID") from error


def _timestamp(value):
    try:
        require(type(value) is str and datetime.fromisoformat(value).utcoffset() is not None,
                "Expected timestamp with timezone")
    except ValueError as error:
        raise ExperimentError("Invalid timestamp") from error


def _sha(value):
    require(type(value) is str and len(value) == 64 and all(c in "0123456789abcdef" for c in value),
            "Invalid SHA256")


def _environment(value):
    value = clone_json(value, 65536)
    keys(value, ("sdk", "python", "platform", "runtime", "executable", "device"))
    require(value["sdk"] == "decker-experiments/1", "Unknown SDK evidence version")
    require(all(type(value[k]) is str and value[k] for k in ("python", "platform")), "Missing host environment")
    require(value["runtime"] is None or type(value["runtime"]) is dict, "Invalid runtime capabilities")
    require(value["device"] is None or type(value["device"]) is dict, "Invalid device description")
    if value["executable"] is not None:
        entry = value["executable"]
        keys(entry, ("path", "size", "sha256"))
        require(type(entry["path"]) is str and entry["path"], "Missing executable path")
        integer(entry["size"], 1, 2**64 - 1)
        _sha(entry["sha256"])
    return value


def environment(*, executable=None, runtime=None, device=None):
    """Capture local environment; unobserved runtime/device information stays null."""
    binary = None
    if executable is not None:
        try:
            path = Path(executable).resolve(strict=True)
            size, sha = 0, hashlib.sha256()
            with path.open("rb") as stream:
                while chunk := stream.read(1024 * 1024):
                    size += len(chunk)
                    sha.update(chunk)
            binary = {"path": str(path), "size": size, "sha256": sha.hexdigest()}
        except OSError as error:
            raise ExperimentError(f"Cannot fingerprint executable: {error}") from error
    return _environment({"sdk": "decker-experiments/1", "python": platform.python_version(),
                         "platform": platform.platform(), "executable": binary, "runtime": runtime, "device": device})


def _no_link(path):
    require(not path.is_symlink() and not (getattr(path.lstat(), "st_file_attributes", 0) & 0x400),
            "Index and artifacts cannot traverse links/reparse points")


def _root(directory):
    path = Path(os.path.abspath(directory))
    for component in [*reversed(path.parents), path]:
        _no_link(component)
    require(path.is_dir(), "Index root must be a directory")
    return path


def _relative_parts(name):
    require(type(name) is str and name and "\\" not in name
            and all(c not in name for c in ':\0<>"|?*') and not any(ord(c) < 32 for c in name),
            "Expected normalized relative path")
    parts = name.split("/")
    for part in parts:
        require(part not in ("", ".", "..") and not part.endswith((".", " "))
                and part.casefold() != ".decker", "Invalid artifact path component")
        stem = part.split(".")[0].casefold()
        require(stem not in {"con", "prn", "aux", "nul", *(f"com{i}" for i in range(1, 10)),
                             *(f"lpt{i}" for i in range(1, 10))}, "Reserved path component")
    return parts


def _inside(root, name):
    parts = _relative_parts(name)
    path = root
    for part in parts:
        path = path / part
        _no_link(path)
    require(path.resolve().is_relative_to(root), "Artifact path escapes index root")
    return path


def _read_file(path, limit):
    _no_link(path)
    require(path.is_file(), "Artifact must be a regular file")
    with _open_read(path) as stream:
        data = stream.read(limit + 1)
    require(len(data) <= limit, "File exceeds byte limit")
    return data


@contextmanager
def _open_read(path):
    # On Windows, os.replace cannot replace an open destination reliably, even
    # with FILE_SHARE_DELETE. Keep the open/read/close and final rename disjoint.
    with snapshot_access(path):
        with path.open("rb") as stream:
            yield stream


def _inspect(root, entry, directory, expected_run_id):
    _uuid(expected_run_id)
    folder = _inside(root, directory)
    require(folder.is_dir(), "Artifact directory is missing")
    names = ["config.json", "metrics.json", "particles.json"]
    if entry["output"]["image"]:
        names.append("image.ppm")
    require({p.name for p in folder.iterdir()} == set(names) | {"provenance.json"}, "Unexpected/missing artifact files")
    contents, stamps = {}, {}
    for name in [*names, "provenance.json"]:
        data = _read_file(folder / name, ARTIFACT_LIMIT)
        contents[name] = data if name == "image.ppm" else decode_json(data, ARTIFACT_LIMIT)
        stamps[name] = {"path": f"{directory}/{name}", "size": len(data), "sha256": hashlib.sha256(data).hexdigest()}
    require(normalize_config(contents["config.json"]) == entry["config"], "Artifact config differs from planned experiment")
    config = entry["config"]
    steps, elapsed = config["steps"], config["steps"] * config["fixed_dt_ns"]
    provenance = contents["provenance.json"]
    keys(provenance, ("run_id", "steps", "simulated_time_ns", "width", "height", "performance",
                      "document_id", "scene_id", "revision", "files"))
    for field in ("run_id", "document_id", "scene_id"):
        _uuid(provenance[field])
    integer(provenance["revision"], 0, 2**64 - 1)
    require(provenance["run_id"] == expected_run_id, "Unexpected source run_id")
    require(provenance["files"] == names, "Provenance file list mismatch")
    for field, expected in (("steps", steps), ("simulated_time_ns", elapsed),
                            ("width", config["view"]["width"]), ("height", config["view"]["height"])):
        require(type(provenance[field]) is int and provenance[field] == expected, f"Provenance {field} mismatch")
    performance = provenance["performance"]
    if performance is not None:
        keys(performance, ("steps", "batch_steps", "wall_time_ns", "scope"))
        integer(performance["steps"], 1, steps)
        integer(performance["batch_steps"], 1, min(8, performance["steps"]))
        integer(performance["wall_time_ns"], 0, 2**63 - 1)
        require(performance["scope"] == "batch_submit_to_completion", "Unknown performance scope")
    report = contents["metrics.json"]
    keys(report, ("steps", "simulated_time_ns", "metrics"))
    require(type(report["steps"]) is int and report["steps"] == steps
            and type(report["simulated_time_ns"]) is int and report["simulated_time_ns"] == elapsed,
            "Metrics do not describe the planned completion boundary")
    metrics = report["metrics"]
    keys(metrics, (*METRICS, "particle_count", "constraint_count", "color_count"))
    particles, constraints = topology(config["cloth"])
    require(integer(metrics["particle_count"], 1, 1024) == particles
            and integer(metrics["constraint_count"], 0, 8192) == constraints, "Metrics topology mismatch")
    integer(metrics["color_count"], 1, 32)
    for name in METRICS:
        finite(metrics[name])
        if name not in ("gravity_potential_energy", "min_height"):
            require(metrics[name] >= 0, f"Negative metric: {name}")
    values = contents["particles.json"]
    require(type(values) is list and len(values) == particles, "Particle array length mismatch")
    for particle in values:
        keys(particle, ("position", "velocity", "inverse_mass"))
        require(0 <= finite(particle["inverse_mass"]) <= 1e6, "Invalid inverse mass")
        for field in ("position", "velocity"):
            require(type(particle[field]) is list and len(particle[field]) == 3
                    and all(abs(finite(v)) <= 1e6 for v in particle[field]), "Invalid particle vector")
    if entry["output"]["image"]:
        width, height = config["view"]["width"], config["view"]["height"]
        header = f"P6\n{width} {height}\n255\n".encode("ascii")
        data = contents["image.ppm"]
        require(data.startswith(header) and len(data) == len(header) + width*height*3,
                "Image dimensions/length mismatch")
    return {"source": provenance, "files": stamps}


def _verify(root):
    value = decode_json(_read_file(root / "index.json", INDEX_LIMIT))
    keys(value, ("format", "version", "created_at", "environment", "plan", "plan_id", "totals", "experiments"))
    require(value["format"] == "DeckerExperimentIndex" and type(value["version"]) is int
            and value["version"] == 1, "Expected DeckerExperimentIndex v1")
    _timestamp(value["created_at"])
    _environment(value["environment"])
    expanded = expand_plan(value["plan"])
    # Canonical equality also distinguishes booleans from integers in cached metadata.
    for field in ("plan", "plan_id", "totals"):
        require(canonical(value[field]) == canonical(expanded[field]), f"Index {field} mismatch")
    require(type(value["experiments"]) is list and len(value["experiments"]) == len(expanded["experiments"]),
            "Index experiment list mismatch")
    paths, attempts = set(), set()
    for stored, expected in zip(value["experiments"], expanded["experiments"]):
        keys(stored, (*expected, "status", "attempts"))
        require(all(canonical(stored[k]) == canonical(v) for k, v in expected.items()), "Index experiment identity mismatch")
        require(type(stored["attempts"]) is list and len(stored["attempts"]) <= 128, "Too many attempts")
        require(stored["status"] == ("verified" if stored["attempts"] else "planned"), "Index status mismatch")
        for attempt in stored["attempts"]:
            keys(attempt, ("attempt_id", "registered_at", "environment", "directory", "source", "files"))
            _uuid(attempt["attempt_id"])
            _timestamp(attempt["registered_at"])
            _environment(attempt["environment"])
            require(type(attempt["directory"]) is str, "Invalid attempt directory")
            alias = attempt["directory"].casefold()
            require(alias not in paths and attempt["attempt_id"] not in attempts, "Duplicate attempt or artifact directory")
            paths.add(alias)
            attempts.add(attempt["attempt_id"])
            require(type(attempt["source"]) is dict and "run_id" in attempt["source"], "Missing attempt source")
            observed = _inspect(root, expected, attempt["directory"], attempt["source"]["run_id"])
            require(canonical(observed) == canonical({k: attempt[k] for k in observed}), "Artifact evidence changed")
    return value


@contextmanager
def _errors():
    try:
        yield
    except OSError as error:
        raise ExperimentError(f"Experiment file operation failed: {error}") from error


def verify_index(directory):
    """Re-expand identities and re-read all registered files; throws on any mismatch."""
    with _errors():
        return _verify(_root(directory))


def _publish(root, value, filename="index.json"):
    data = canonical(clone_json(value)) + b"\n"
    require(len(data) <= INDEX_LIMIT, "Index exceeds 32 MiB")
    descriptor, name = tempfile.mkstemp(prefix=".index-", suffix=".tmp", dir=root)
    try:
        with os.fdopen(descriptor, "wb") as stream:
            stream.write(data)
            stream.flush()
            os.fsync(stream.fileno())
        with snapshot_access(root / filename):
            os.replace(name, root / filename)
    finally:
        if os.path.exists(name):
            os.unlink(name)


def create_index(directory, plan, *, execution_environment=None):
    """Create only a new directory. Invalid plans have no filesystem side effects."""
    expanded = expand_plan(plan)
    observed = environment() if execution_environment is None else _environment(execution_environment)
    value = {"format": "DeckerExperimentIndex", "version": 1, "created_at": _now(),
             "environment": observed, **expanded}
    value["experiments"] = [{**entry, "status": "planned", "attempts": []} for entry in expanded["experiments"]]
    with _errors():
        destination = Path(os.path.abspath(directory))
        _root(destination.parent)
        destination.mkdir()  # exclusive ownership; never remove an existing directory
        try:
            _publish(destination, value)
        except BaseException:
            destination.rmdir()
            raise
    return value


def register_artifacts(directory, experiment_id, artifact_directory, *, expected_run_id, execution_environment):
    """Append independently verified evidence, atomically retaining all previous attempts."""
    observed_environment = _environment(execution_environment)
    with _errors():
        root = _root(directory)
        lock = root / ".index.lock"
        descriptor = os.open(lock, os.O_CREAT | os.O_EXCL | os.O_WRONLY)
        try:
            with os.fdopen(descriptor, "w", encoding="utf-8") as stream:
                stream.write(str(os.getpid()))
            current = _verify(root)
            entries = [e for e in current["experiments"] if e["experiment_id"] == experiment_id]
            require(len(entries) == 1, "Unknown experiment_id")
            entry = entries[0]
            require(len(entry["attempts"]) < 128, "Attempt limit exceeded")
            require(type(artifact_directory) is str, "Artifact directory must be relative text")
            used = {a["directory"].casefold() for e in current["experiments"] for a in e["attempts"]}
            require(artifact_directory.casefold() not in used, "Artifact directory already registered")
            evidence = _inspect(root, entry, artifact_directory, expected_run_id)
            entry["attempts"].append({"attempt_id": str(uuid4()), "registered_at": _now(),
                                      "environment": observed_environment, "directory": artifact_directory, **evidence})
            entry["status"] = "verified"
            _publish(root, current)
            return entry["attempts"][-1]
        finally:
            lock.unlink()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    args = parser.parse_args()
    try:
        index = verify_index(args.directory)
        print(json.dumps({"plan_id": index["plan_id"], "totals": index["totals"],
                          "verified": sum(e["status"] == "verified" for e in index["experiments"])}))
    except ExperimentError as error:
        parser.exit(2, f"{error}\n")


if __name__ == "__main__":
    main()
