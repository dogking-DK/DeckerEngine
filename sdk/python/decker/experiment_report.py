"""Verified offline comparisons and immutable JSON/CSV/HTML experiment reports."""

import argparse
from contextlib import ExitStack
import hashlib
import json
import math
import os
from pathlib import Path
import tempfile
from uuid import uuid4

from ._experiment_lock import exclusive_path
from .experiment_batch import read_progress
from .experiment_index import (ARTIFACT_LIMIT, METRICS, _errors, _index_writer, _inside, _now,
                               _read_file, _root, verify_index)
from .experiments import (INDEX_LIMIT, canonical, clone_json, decode_json, finite, integer,
                          keys, read_json, require)

DEFAULT_TOLERANCES = {"format": "DeckerExperimentTolerances", "version": 1,
    "same_solver": {"position_abs": 1e-6, "velocity_abs": 1e-5},
    "cross_solver": {"position_abs": 2e-3, "velocity_abs": 2e-2}}
MEASUREMENT_NOTES = [
    "last_batch is the most recent batch_submit_to_completion wall-clock sample, not the entire solve.",
    "Samples include submission/completion overhead; they are not GPU timestamps or steady-state statistics.",
    "CLI end-to-end covers this run/resume invocation through Stop, including collection and IO, excluding report generation.",
    "Missing measurements remain null. Recorded environment equality does not establish equality of unknown hardware.",
    "Numerical gates use maximum absolute particle component errors and exact inverse masses; metric deltas are descriptive.",
    "Timing differences are observations, not statistically established speedups. Images are indexed, not pixel-compared."]


def tolerance_policy(value=None):
    value = clone_json(DEFAULT_TOLERANCES if value is None else value, 4096)
    keys(value, ("format", "version", "same_solver", "cross_solver"))
    require(value["format"] == "DeckerExperimentTolerances" and type(value["version"]) is int
            and value["version"] == 1, "Expected DeckerExperimentTolerances v1")
    for kind in ("same_solver", "cross_solver"):
        keys(value[kind], ("position_abs", "velocity_abs"))
        for number in value[kind].values():
            require(0 <= finite(number) <= 1e6, "Tolerance must be finite and nonnegative, at most 1e6")
    return value


def _stamp(data):
    return {"size": len(data), "sha256": hashlib.sha256(data).hexdigest()}


def _artifact(root, row, name):
    stamp = row["evidence"]["files"][name]
    data = _read_file(_inside(root, stamp["path"]), ARTIFACT_LIMIT)
    require(_stamp(data) == {k: stamp[k] for k in ("size", "sha256")}, "Artifact changed during reporting")
    return decode_json(data, ARTIFACT_LIMIT)


def _snapshot(root):
    batch = read_progress(root)
    index = verify_index(root)
    stamps = {name: _stamp(_read_file(root / name, INDEX_LIMIT)) for name in ("batch.json", "index.json")}
    rows = []
    for item, entry in zip(batch["experiments"], index["experiments"]):
        attempt = next((a for a in entry["attempts"] if a["attempt_id"] == item["artifact_attempt_id"]), None)
        row = {"experiment_id": entry["experiment_id"], "config": entry["config"], "output": entry["output"],
            "status": item["status"], "error": item["error"], "run_id": item["run_id"],
            "attempt_id": item["artifact_attempt_id"], "history": item.get("history", []),
            "registered_attempts": entry["attempts"], "evidence": attempt, "metrics": None,
            "last_batch": None, "gpu_timestamp_ns": None}
        # A collected failed attempt is still a failed attempt, never a substitute for a success.
        if item["status"] == "succeeded":
            row["metrics"] = _artifact(root, row, "metrics.json")["metrics"]
            row["last_batch"] = attempt["source"]["performance"]
        rows.append(row)
    return {"directory": str(root), "batch_id": batch["batch_id"], "plan_id": batch["plan_id"],
        "sequence": batch["sequence"], "status": batch["status"], "source": batch["source"],
        "environment": index["environment"], "plan": index["plan"], "files": stamps, "experiments": rows}


def _reference(snapshot, row):
    return None if row is None else {"batch_id": snapshot["batch_id"], "experiment_id": row["experiment_id"],
        "attempt_id": row["attempt_id"], "run_id": row["run_id"], "status": row["status"]}


def _difference(a, b):
    # Inputs can contain very large finite metrics. Overflow is missing, never JSON Infinity.
    result = b - a
    return result if math.isfinite(result) else None


def _compare(left, a, right, b, kind, tolerance):
    result = {"kind": kind, "left": _reference(left, a), "right": _reference(right, b),
        "status": "incomplete", "reason": None, "tolerance": tolerance,
        "recorded_environment_equal": None, "position_max_abs": None, "position_rms": None,
        "velocity_max_abs": None, "velocity_rms": None, "inverse_mass_equal": None,
        "metric_deltas": None, "last_batch_delta_ns": None, "last_batch_ratio": None}
    if a is None or b is None or a["status"] != "succeeded" or b["status"] != "succeeded":
        result["reason"] = "Missing counterpart or current attempt is not succeeded"
        return result
    require(a["run_id"] != b["run_id"], "Cannot compare a run with itself")
    result["recorded_environment_equal"] = a["evidence"]["environment"] == b["evidence"]["environment"]
    particles_a = _artifact(Path(left["directory"]), a, "particles.json")
    particles_b = _artifact(Path(right["directory"]), b, "particles.json")
    require(len(particles_a) == len(particles_b), "Comparison topology differs")
    for field in ("position", "velocity"):
        differences = [abs(x - y) for pa, pb in zip(particles_a, particles_b)
                       for x, y in zip(pa[field], pb[field])]
        result[field + "_max_abs"] = max(differences)
        result[field + "_rms"] = math.sqrt(math.fsum(d*d for d in differences) / len(differences))
    result["inverse_mass_equal"] = all(pa["inverse_mass"] == pb["inverse_mass"]
                                       for pa, pb in zip(particles_a, particles_b))
    passed = (result["position_max_abs"] <= tolerance["position_abs"]
              and result["velocity_max_abs"] <= tolerance["velocity_abs"] and result["inverse_mass_equal"])
    result.update(status="passed" if passed else "failed", reason=None if passed else "Particle tolerance exceeded or inverse mass differs")
    result["metric_deltas"] = {name: _difference(a["metrics"][name], b["metrics"][name]) for name in METRICS}
    p, q = a["last_batch"], b["last_batch"]
    if p and q and (p["steps"], p["batch_steps"]) == (q["steps"], q["batch_steps"]):
        result["last_batch_delta_ns"] = q["wall_time_ns"] - p["wall_time_ns"]
        if p["wall_time_ns"]:
            result["last_batch_ratio"] = q["wall_time_ns"] / p["wall_time_ns"]
    return result


def _comparisons(current, baseline, policy):
    comparisons = []
    groups = {}
    for row in current["experiments"]:
        config = dict(row["config"])
        solver = config.pop("solver")
        key = canonical({"config": config, "output": row["output"]})
        groups.setdefault(key, {})[solver] = row
    if set(current["plan"]["solvers"]) == {"xpbd_cpu", "xpbd_gpu"}:
        for group in groups.values():
            comparisons.append(_compare(current, group.get("xpbd_cpu"), current, group.get("xpbd_gpu"),
                                        "cpu_gpu", policy["cross_solver"]))
    if baseline is not None:
        old = {r["experiment_id"]: r for r in baseline["experiments"]}
        new = {r["experiment_id"]: r for r in current["experiments"]}
        for identity in dict.fromkeys([*new, *old]):
            comparisons.append(_compare(baseline, old.get(identity), current, new.get(identity),
                                        "rerun", policy["same_solver"]))
    return comparisons


def _observation(value):
    if value is None:
        return None
    value = clone_json(value, 4096)
    keys(value, ("scope", "action", "wall_time_ns"))
    require(value["scope"] == "batch_cli_invocation" and value["action"] in ("run", "resume"),
            "Unknown end-to-end observation scope")
    integer(value["wall_time_ns"], 0, 2**63-1)
    return value


def write_report(directory, *, baseline=None, tolerances=None, execution_observation=None):
    """Reverify and publish a new report directory. Never mutates batch/index/artifacts."""
    policy = tolerance_policy(tolerances)
    observation = _observation(execution_observation)
    with _errors(), ExitStack() as stack:
        root = _root(directory)
        other = _root(baseline) if baseline is not None else None
        require(other is None or other != root, "Baseline must be an independent batch")
        for source in sorted([root] + ([other] if other is not None else []), key=lambda p: os.path.normcase(str(p))):
            stack.enter_context(exclusive_path("batch", source))
            stack.enter_context(_index_writer(source))
        current = _snapshot(root)
        previous = _snapshot(other) if other is not None else None
        require(previous is None or previous["batch_id"] != current["batch_id"], "Baseline must be an independent batch")
        comparisons = _comparisons(current, previous, policy)
        states = [pair["status"] for pair in comparisons]
        status = ("failed" if "failed" in states else "incomplete" if "incomplete" in states or
                  any(r["status"] != "succeeded" for r in current["experiments"]) else
                  "passed" if comparisons else "not_compared")
        report = {"format": "DeckerExperimentReport", "version": 1, "report_id": str(uuid4()),
            "created_at": _now(), "comparison_status": status, "tolerances": policy,
            "current": current, "baseline": previous, "comparisons": comparisons,
            "execution_observation": observation, "measurement_notes": MEASUREMENT_NOTES}
        from ._experiment_report_render import render_files
        files = render_files(report)
        # Bound report output independently of source artifact limits.
        require(sum(len(data) for data in files.values()) <= 128 * 1024 * 1024, "Report exceeds 128 MiB")
        parent = root / "reports"
        parent.mkdir(exist_ok=True)
        _root(parent)
        target = parent / report["report_id"]
        temporary = Path(tempfile.mkdtemp(prefix=".report-", dir=parent))
        try:
            for name, data in files.items():
                with (temporary / name).open("xb") as stream:
                    stream.write(data)
                    stream.flush()
                    os.fsync(stream.fileno())
            temporary.rename(target)
        finally:
            if temporary.exists():
                # Only the known files created above are removed; never recursively delete source evidence.
                for name in files:
                    (temporary / name).unlink(missing_ok=True)
                temporary.rmdir()
        return {"directory": str(target), "report_id": report["report_id"], "comparison_status": status,
                "comparisons": len(comparisons)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    parser.add_argument("--baseline", type=Path)
    parser.add_argument("--tolerances", type=Path)
    args = parser.parse_args()
    from .experiments import ExperimentError
    try:
        result = write_report(args.directory, baseline=args.baseline,
                              tolerances=read_json(args.tolerances, 4096) if args.tolerances else None)
        print(json.dumps(result, ensure_ascii=False))
        if result["comparison_status"] in ("failed", "incomplete"):
            parser.exit(1)
    except ExperimentError as error:
        parser.exit(2, f"{error}\n")


if __name__ == "__main__":
    main()
