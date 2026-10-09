"""Run/replay an exact-step cloth experiment on an existing, exclusively controlled pipe host."""
import argparse
import json
from pathlib import Path

from decker import Client, RpcError, guard

VIEW = [1.2, 0, .4, -.2, 0, -.9, .65, .15, 0, -.325, -.45, .8, 0, 0, 0, 1]


def validate(config):
    if (not isinstance(config, dict) or set(config) != {
            "format", "version", "solver", "steps", "fixed_dt_ns", "cloth", "view"}
            or config["format"] != "DeckerSimulationExperiment" or type(config["version"]) is not int
            or config["version"] != 1 or config["solver"] not in ("xpbd_cpu", "xpbd_gpu")
            or type(config["steps"]) is not int or not 0 <= config["steps"] <= 10000
            or type(config["fixed_dt_ns"]) is not int or not 1000000 <= config["fixed_dt_ns"] <= 33333333
            or not isinstance(config["cloth"], dict)):
        raise ValueError("Expected a DeckerSimulationExperiment v1 config (0..10000 steps)")
    view = config["view"]
    if (not isinstance(view, dict) or set(view) != {"width", "height", "view_projection"}
            or any(type(view[k]) is not int or not 1 <= view[k] <= 2048 for k in ("width", "height"))
            or not isinstance(view["view_projection"], list) or len(view["view_projection"]) != 16
            or any(type(a) not in (int, float) or not abs(a-b) <= 1e-7
                   for a, b in zip(view["view_projection"], VIEW))):
        raise ValueError("Experiment v1 uses the fixed cloth view and dimensions 1..2048")


def run_experiment(client, config, output):
    """Leaves an active run on failure for inspection; never retries ambiguous commands."""
    validate(config)
    caps = client.call("runtime.capabilities").value["simulation"]
    if not caps.get("experiment_export") or (config["solver"] == "xpbd_gpu" and not caps.get("gpu")):
        raise ValueError("The host lacks the requested experiment capability")
    state = client.call("scene.query").value["state"]
    run = client.call("simulation.start", {"guard": guard(state), "paused": True,
        "solver": config["solver"], "fixed_dt_ns": config["fixed_dt_ns"], "cloth": config["cloth"]}).value["run"]
    identity = {"run_id": run["run_id"]}
    for first in range(0, config["steps"], 8):
        count = min(8, config["steps"]-first)
        run = client.call("simulation.step", {**identity, "count": count}).value["run"]
        if run["steps"] != first+count or run["fault"] is not None:
            raise RuntimeError("Simulation failed to advance the requested exact count")
    result = client.call("simulation.export", {**identity, "expected_steps": config["steps"],
        "output": output, "width": config["view"]["width"], "height": config["view"]["height"]}).value
    if result["steps"] != config["steps"] or result["simulated_time_ns"] != config["steps"]*config["fixed_dt_ns"]:
        raise RuntimeError("Export returned an unexpected simulation version")
    client.call("simulation.stop", identity)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pipe", required=True)
    parser.add_argument("--ctl", default="dk-ctl")
    parser.add_argument("--output", required=True, help="New directory relative to the host project")
    parser.add_argument("--config", type=Path, help="Replay a previously exported config.json from step zero")
    parser.add_argument("--solver", choices=("xpbd_cpu", "xpbd_gpu"), help="Optional backend override for comparison")
    parser.add_argument("--steps", type=int, default=300, help="Used only without --config")
    parser.add_argument("--seed", type=int, default=42, help="Used only without --config")
    args = parser.parse_args()
    config = json.loads(args.config.read_text(encoding="utf-8")) if args.config else {
        "format": "DeckerSimulationExperiment", "version": 1, "solver": "xpbd_gpu", "steps": args.steps,
        "fixed_dt_ns": 10000000, "cloth": {"seed": args.seed},
        "view": {"width": 640, "height": 480, "view_projection": VIEW}}
    if args.solver:
        config["solver"] = args.solver
    validate(config)
    client = Client(args.pipe, args.ctl, timeout=60)
    try:
        client.call("scene.query")
    except RpcError as error:
        if error.code != -32002:
            raise
        client.call("scene.new", {"name": "Cloth experiment"})
    print(json.dumps(run_experiment(client, config, args.output), ensure_ascii=False))


if __name__ == "__main__":
    main()
