"""M13.1 plan/evidence interoperability, using real commands (not a batch executor)."""

import argparse
import json
from pathlib import Path
from uuid import uuid4

from decker import RpcError, guard
from decker.experiments import ExperimentError, canonical, expand_plan
from decker.experiment_index import create_index, environment, register_artifacts, verify_index
from PythonClientTest import server


def numeric_evidence(client, root, entry, run):
    """Fixture-only serialization of existing CPU queries; no production exporter claimed."""
    identity = {"run_id": run["run_id"]}
    config = entry["config"]
    sample = client.call("simulation.sample", {**identity, "expected_steps": config["steps"]}).value["run"]["metrics_sample"]
    particles, pages = [], []
    while True:
        page = client.call("simulation.particles", {**identity, "offset": len(particles), "limit": 16}).value
        assert page["run_id"] == run["run_id"] and page["steps"] == config["steps"]
        assert page["offset"] == len(particles) and page["simulated_time_ns"] == sample["simulated_time_ns"]
        particles.extend({k: v for k, v in p.items() if k != "index"} for p in page["particles"])
        pages.append(page)
        if not page["has_more"]:
            break
    assert client.call("simulation.query").value["run"]["steps"] == config["steps"]
    source = run["source"]
    root.mkdir()
    files = {"config.json": config, "metrics.json": sample, "particles.json": particles,
        "provenance.json": {**identity, "steps": config["steps"], "simulated_time_ns": sample["simulated_time_ns"],
            "document_id": source["document_id"], "scene_id": source["scene_id"], "revision": source["revision"],
            "width": config["view"]["width"], "height": config["view"]["height"], "performance": None,
            "files": ["config.json", "metrics.json", "particles.json"]}}
    for name, value in files.items():
        (root / name).write_bytes(canonical(value))
    return {"sample": sample, "pages": pages}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("runner", "client", "work-dir"):
        parser.add_argument("--" + name, required=True)
    parser.add_argument("--gpu", action="store_true")
    args = parser.parse_args()
    args.pipe_timeout_ms = 60000
    root = Path(args.work_dir).resolve() / uuid4().hex
    root.mkdir(parents=True)
    plan = {"format": "DeckerExperimentPlan", "version": 1,
        "base": {"steps": 3, "fixed_dt_ns": 10000000, "cloth": {"columns": 4, "rows": 4, "iterations": 4}},
        "sweep": {"cloth.spacing": [.1, .15]}, "seeds": [42],
        "solvers": ["xpbd_gpu" if args.gpu else "xpbd_cpu"],
        "output": {"image": args.gpu, "width": 96, "height": 72},
        "budget": {"max_experiments": 2, "max_total_steps": 6, "max_work_units": 100000}}
    expanded = expand_plan(plan)
    assert expanded == expand_plan(json.loads(json.dumps(plan, sort_keys=True)))
    (root / "plan.json").write_bytes(canonical(plan))
    with server(args, root, "dk-experiment-index-" + uuid4().hex) as client:
        client.timeout = 60
        caps = client.call("runtime.capabilities").value
        env = environment(executable=args.runner, runtime=caps)
        index_root = root / "index"
        index = create_index(index_root, plan, execution_environment=env)
        state = client.call("scene.new", {"name": "M13.1 provenance"}).value
        for ordinal, entry in enumerate(index["experiments"]):
            config = entry["config"]
            try:
                run = client.call("simulation.start", {"guard": guard(state), "paused": True,
                    "solver": config["solver"], "fixed_dt_ns": config["fixed_dt_ns"], "cloth": config["cloth"]}).value["run"]
            except RpcError as error:
                if (args.gpu and error.data.get("engine_name") in ("not_found", "not_supported")
                        and "simulation.gpu.device" in error.data.get("context", [])):
                    print(f"GPU unavailable: {error}")
                    raise SystemExit(77)
                raise
            assert run["cloth"] == config["cloth"], (run["cloth"], config["cloth"])
            run = client.call("simulation.step", {"run_id": run["run_id"], "count": config["steps"]}).value["run"]
            assert run["steps"] == config["steps"] and run["fault"] is None
            name = f"result-{ordinal}"
            if args.gpu:
                receipt = client.call("simulation.export", {"run_id": run["run_id"], "expected_steps": config["steps"],
                    "output": "index/" + name, "width": config["view"]["width"], "height": config["view"]["height"]}).value
                assert receipt["run_id"] == run["run_id"] and receipt["steps"] == config["steps"]
            else:
                receipt = numeric_evidence(client, index_root / name, entry, run)
            (root / f"receipt-{ordinal}.json").write_bytes(canonical({"run": run, "receipt": receipt}))
            before = (index_root / "index.json").read_bytes()
            try:
                register_artifacts(index_root, entry["experiment_id"], name,
                                   expected_run_id=str(uuid4()), execution_environment=env)
                raise AssertionError("Wrong run_id accepted")
            except ExperimentError:
                assert (index_root / "index.json").read_bytes() == before
            attempt = register_artifacts(index_root, entry["experiment_id"], name,
                                         expected_run_id=run["run_id"], execution_environment=env)
            assert attempt["source"]["revision"] == run["source"]["revision"]
            assert attempt["environment"]["executable"]["sha256"] == env["executable"]["sha256"]
            client.call("simulation.stop", {"run_id": run["run_id"]})
        checked = verify_index(index_root)
        assert checked["plan_id"] == expanded["plan_id"]
        assert all(e["status"] == "verified" for e in checked["experiments"])
        assert client.call("scene.query").value["state"] == state
        report = {"plan_id": checked["plan_id"], "totals": checked["totals"], "gpu": args.gpu,
                  "runtime_capabilities": caps, "verified_attempts": 2, "runner_sha256": env["executable"]["sha256"]}
        (root / "report.json").write_bytes(canonical(report))
    print(f"Stable plan, exact configs, independent run IDs, artifact stamps and source verification passed: {root}")


if __name__ == "__main__":
    main()
