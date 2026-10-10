"""Versioned, offline experiment plans. No engine calls or implicit execution."""

import argparse
import hashlib
import itertools
import json
import math
from pathlib import Path
import struct

from .models import ClientError

PLAN_LIMIT = 1024 * 1024
INDEX_LIMIT = 32 * 1024 * 1024
VIEW = [1.2, 0, .4, -.2, 0, -.9, .65, .15, 0, -.325, -.45, .8, 0, 0, 0, 1]
DEFAULT_CLOTH = dict(columns=8, rows=8, spacing=.15, height=.75, particle_mass=.1,
                     compliance=1e-6, iterations=12, gravity_y=-9.81, floor_y=0, damping=.5)
INTEGER_FIELDS = {"columns": (2, 32), "rows": (2, 32), "iterations": (1, 32),
                  "seed": (0, 2**32 - 1)}
FLOAT_FIELDS = {"spacing": (.01, 1), "height": (-100, 100), "particle_mass": (.001, 100),
                "compliance": (0, .01), "gravity_y": (-1000, 1000),
                "floor_y": (-10000, 10000), "damping": (0, 100)}
BUDGET_LIMITS = dict(max_experiments=4096, max_total_steps=10**9, max_work_units=10**12)


class ExperimentError(ClientError):
    """Invalid plan, index, artifact evidence, or publication failure."""


def require(condition, message):
    if not condition:
        raise ExperimentError(message)


def keys(value, required, optional=()):
    require(type(value) is dict and set(required) <= value.keys()
            and value.keys() <= set(required) | set(optional), "Invalid or unknown fields")


def integer(value, low, high):
    require(type(value) is int and low <= value <= high, f"Expected integer {low}..{high}")
    return value


def finite(value):
    require(type(value) in (int, float) and math.isfinite(value), "Expected finite number")
    return value


def f32(value):
    finite(value)
    try:
        value = struct.unpack("<f", struct.pack("<f", value))[0]
    except (OverflowError, struct.error) as error:
        raise ExperimentError("Value is outside float32 range") from error
    require(math.isfinite(value), "Value is outside float32 range")
    return value if value else 0.0


def canonical(value):
    return json.dumps(value, sort_keys=True, ensure_ascii=False, allow_nan=False,
                      separators=(",", ":")).encode("utf-8")


def digest(value):
    return hashlib.sha256(canonical(value)).hexdigest()


def clone_json(value, limit=INDEX_LIMIT):
    """Bound recursion and aggregate strings before encoding user supplied objects."""
    nodes, strings = 0, 0

    def visit(item, depth=0):
        nonlocal nodes, strings
        nodes += 1
        require(depth <= 24 and nodes <= (100000 if limit <= PLAN_LIMIT else 2000000), "JSON structure exceeds limits")
        if type(item) is str:
            strings += len(item)
            require(strings <= limit, "JSON strings exceed limits")
        elif item is None or type(item) is bool:
            pass
        elif type(item) is int:
            require(abs(item) <= 2**64 - 1, "JSON integer exceeds uint64 magnitude")
        elif type(item) is float:
            finite(item)
        elif type(item) is list:
            for child in item:
                visit(child, depth + 1)
        elif type(item) is dict:
            for name, child in item.items():
                require(type(name) is str, "JSON object keys must be strings")
                visit(name, depth + 1)
                visit(child, depth + 1)
        else:
            raise ExperimentError("Expected native JSON data")

    try:
        visit(value)
        data = canonical(value)
        require(len(data) <= limit, "JSON byte size exceeds limit")
        return json.loads(data)
    except (ValueError, UnicodeError, RecursionError) as error:
        raise ExperimentError("Invalid JSON data") from error


def decode_json(data, limit=INDEX_LIMIT):
    def pairs(items):
        result = {}
        for key, value in items:
            require(key not in result, f"Duplicate JSON key: {key}")
            result[key] = value
        return result

    def constant(value):
        raise ExperimentError(f"Non-finite JSON constant: {value}")

    try:
        require(len(data) <= limit, "JSON byte size exceeds limit")
        result = json.loads(data.decode("utf-8"), object_pairs_hook=pairs, parse_constant=constant)
        return clone_json(result, limit)
    except (ValueError, UnicodeError, RecursionError) as error:
        raise ExperimentError(f"Cannot decode JSON: {error}") from error


def read_json(path, limit=INDEX_LIMIT):
    try:
        with Path(path).open("rb") as stream:
            return decode_json(stream.read(limit + 1), limit)
    except OSError as error:
        raise ExperimentError(f"Cannot read JSON: {path}: {error}") from error


def cloth_value(name, value):
    if name in INTEGER_FIELDS:
        return integer(value, *INTEGER_FIELDS[name])
    require(name in FLOAT_FIELDS, f"Unknown cloth field: {name}")
    low, high = map(f32, FLOAT_FIELDS[name])
    # Command schema compares incoming double values to float32 endpoints.
    require(low <= finite(value) <= high, f"Cloth {name} is out of range")
    return f32(value)


def cloth_config(value, *, seed=True, complete=False):
    fields = set(DEFAULT_CLOTH) | ({"seed"} if seed else set())
    keys(value, fields if complete else (), fields)
    result = {name: cloth_value(name, value.get(name, default))
              for name, default in DEFAULT_CLOTH.items()}
    if seed:
        result["seed"] = cloth_value("seed", value.get("seed", 1))
    require(result["height"] > result["floor_y"], "Cloth height must be above floor_y")
    return result


def normalize_config(value):
    keys(value, ("format", "version", "solver", "steps", "fixed_dt_ns", "cloth", "view"))
    require(value["format"] == "DeckerSimulationExperiment" and type(value["version"]) is int
            and value["version"] == 1, "Expected DeckerSimulationExperiment v1")
    require(value["solver"] in ("xpbd_cpu", "xpbd_gpu"), "Unknown solver")
    integer(value["steps"], 0, 1000000)
    integer(value["fixed_dt_ns"], 1000000, 33333333)
    view = value["view"]
    keys(view, ("width", "height", "view_projection"))
    for dimension in ("width", "height"):
        integer(view[dimension], 1, 2048)
    require(type(view["view_projection"]) is list and len(view["view_projection"]) == 16
            and all(f32(a) == f32(b) for a, b in zip(view["view_projection"], VIEW)),
            "Experiment v1 requires the fixed cloth camera")
    return {**value, "cloth": cloth_config(value["cloth"], complete=True),
            "view": {**view, "view_projection": list(map(f32, VIEW))}}


def topology(cloth):
    c, r = cloth["columns"], cloth["rows"]
    return c*r, r*(c-1) + c*(r-1) + 2*(c-1)*(r-1)


def _axis(name, value):
    if name == "steps":
        return integer(value, 0, 1000000)
    if name == "fixed_dt_ns":
        return integer(value, 1000000, 33333333)
    require(name.startswith("cloth.") and name[6:] in DEFAULT_CLOTH, f"Unknown sweep axis: {name}")
    return cloth_value(name[6:], value)


def _values(values, convert):
    require(type(values) is list and 1 <= len(values) <= 4096, "Axis needs 1..4096 values")
    result = [convert(value) for value in values]
    require(len({digest(value) for value in result}) == len(result), "Duplicate normalized axis values")
    return result


def expand_plan(value):
    """Validate the entire plan before returning deterministic entries; never does IO."""
    value = clone_json(value, PLAN_LIMIT)
    keys(value, ("format", "version", "base", "sweep", "seeds", "solvers", "output", "budget"))
    require(value["format"] == "DeckerExperimentPlan" and type(value["version"]) is int
            and value["version"] == 1, "Expected DeckerExperimentPlan v1")
    base = value["base"]
    keys(base, ("steps", "fixed_dt_ns", "cloth"))
    base = {"steps": _axis("steps", base["steps"]), "fixed_dt_ns": _axis("fixed_dt_ns", base["fixed_dt_ns"]),
            "cloth": cloth_config(base["cloth"], seed=False)}
    require(type(value["sweep"]) is dict, "sweep must be an object")
    sweep = {name: _values(values, lambda v: _axis(name, v)) for name, values in sorted(value["sweep"].items())}
    seeds = _values(value["seeds"], lambda v: cloth_value("seed", v))

    def solver(v):
        require(v in ("xpbd_cpu", "xpbd_gpu"), "Unknown solver")
        return v

    solvers = _values(value["solvers"], solver)
    output = value["output"]
    keys(output, ("image", "width", "height"))
    require(type(output["image"]) is bool, "output.image must be boolean")
    for dimension in ("width", "height"):
        integer(output[dimension], 1, 2048)
    budget = value["budget"]
    keys(budget, BUDGET_LIMITS)
    for name, hard in BUDGET_LIMITS.items():
        integer(budget[name], 1, hard)
    count = math.prod(map(len, sweep.values())) * len(seeds) * len(solvers)
    require(count <= budget["max_experiments"], "Experiment count exceeds budget")
    plan = {**value, "base": base, "sweep": sweep, "seeds": seeds, "solvers": solvers}
    entries, ids = [], set()
    total_steps, work_units = 0, 0
    for combination in itertools.product(*sweep.values()):
        config_base = {**base, "cloth": dict(base["cloth"])}
        for name, v in zip(sweep, combination):
            if name.startswith("cloth."):
                config_base["cloth"][name[6:]] = v
            else:
                config_base[name] = v
        for backend, seed in itertools.product(solvers, seeds):
            config = normalize_config({"format": "DeckerSimulationExperiment", "version": 1,
                **config_base, "solver": backend, "cloth": {**config_base["cloth"], "seed": seed},
                "view": {"width": output["width"], "height": output["height"], "view_projection": VIEW}})
            particles, constraints = topology(config["cloth"])
            work = config["steps"] * (particles + config["cloth"]["iterations"] * (particles + constraints))
            total_steps += config["steps"]
            work_units += work
            require(total_steps <= budget["max_total_steps"], "Total steps exceed budget")
            require(work_units <= budget["max_work_units"], "Solver work exceeds budget")
            identity = digest({"config": config, "output": output})
            require(identity not in ids, "Duplicate experiment after normalization")
            ids.add(identity)
            entries.append({"experiment_id": identity, "config": config, "output": dict(output), "work_units": work})
    return {"plan": plan, "plan_id": digest(plan), "experiments": entries,
            "totals": {"experiments": count, "steps": total_steps, "work_units": work_units}}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("plan", type=Path)
    parser.add_argument("--output", type=Path, help="Create a new index directory after full validation")
    args = parser.parse_args()
    try:
        expanded = expand_plan(read_json(args.plan, PLAN_LIMIT))
        if args.output:
            from .experiment_index import create_index
            create_index(args.output, expanded["plan"])
        print(json.dumps(expanded, ensure_ascii=False, indent=2))
    except ExperimentError as error:
        parser.exit(2, f"{error}\n")


if __name__ == "__main__":
    main()
