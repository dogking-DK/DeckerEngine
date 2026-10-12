"""M13.4: real parameter/seed scans, automatic reports and independent 300-step reruns."""

import argparse
import csv
import hashlib
import json
from pathlib import Path
import subprocess
import sys
from uuid import uuid4

from decker.experiments import canonical
from PythonClientTest import server


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("runner", "client", "work-dir"):
        parser.add_argument("--" + name, required=True)
    parser.add_argument("--gpu", action="store_true")
    parser.add_argument("--cpu-only", action="store_true")
    args = parser.parse_args()
    args.pipe_timeout_ms = 60000
    root = Path(args.work_dir).resolve() / uuid4().hex
    root.mkdir(parents=True)
    plan = {"format": "DeckerExperimentPlan", "version": 1,
        "base": {"steps": 300, "fixed_dt_ns": 10000000, "cloth": {"columns": 8, "rows": 8}},
        "sweep": {"cloth.iterations": [8, 12]}, "seeds": [42, 43],
        "solvers": ["xpbd_cpu", "xpbd_gpu"] if args.gpu else ["xpbd_cpu"],
        "output": {"image": args.gpu, "width": 96, "height": 72},
        "budget": {"max_experiments": 8, "max_total_steps": 2400, "max_work_units": 10**8}}
    plan_path = root / "plan.json"
    plan_path.write_bytes(canonical(plan))
    endpoint = "dk-report-" + uuid4().hex
    command = [sys.executable, "-m", "decker.experiment_batch", "run", str(plan_path),
        "--pipe", endpoint, "--ctl", args.client, "--project-root", str(root), "--runner", args.runner,
        "--poll-interval", ".01", "--experiment-timeout", "90"]
    results = []
    with server(args, root, endpoint) as client:
        client.timeout = 60
        client.call("scene.new", {"name": "M13.4 scan and rerun"})
        source = client.call("scene.query").value
        history = client.call("history.status").value
        if args.cpu_only:
            caps = client.call("runtime.capabilities").value
            assert caps["simulation"]["gpu"] is False and caps["simulation"]["experiment_export"] is False
        for name in ("first", "rerun"):
            extra = ["--baseline", str(root / "first")] if name == "rerun" else []
            if extra:
                # Different plan ordering must still pair the exact seed/parameters by stable identity.
                plan["seeds"].reverse()
                plan["sweep"]["cloth.iterations"].reverse()
                plan_path.write_bytes(canonical(plan))
            with (root / f"{name}.progress.jsonl").open("wb") as errors:
                completed = subprocess.run([*command, "--output", name, *extra], stdout=subprocess.PIPE,
                    stderr=errors, timeout=240, creationflags=subprocess.CREATE_NO_WINDOW)
            (root / f"{name}.summary.json").write_bytes(completed.stdout)
            assert completed.returncode == 0, completed.stdout.decode("utf-8")
            summary = json.loads(completed.stdout)
            assert summary["status"] == "succeeded" and "report_error" not in summary
            result = summary["comparison_report"]
            report_dir = Path(result["directory"])
            report = json.loads((report_dir / "report.json").read_bytes())
            results.append(result)
            expected_status = "passed" if args.gpu or extra else "not_compared"
            assert report["comparison_status"] == expected_status
            assert report["execution_observation"]["scope"] == "batch_cli_invocation"
            assert report["execution_observation"]["wall_time_ns"] > 0
            expected_count = 8 if args.gpu else 4
            assert len(report["current"]["experiments"]) == expected_count
            for row in report["current"]["experiments"]:
                assert row["status"] == "succeeded" and row["metrics"]["particle_count"] == 64
                assert row["config"]["steps"] == row["last_batch"]["steps"] == 300
                assert row["last_batch"]["batch_steps"] == 4 and row["last_batch"]["scope"] == "batch_submit_to_completion"
                assert row["gpu_timestamp_ns"] is None
                assert ("image.ppm" in row["evidence"]["files"]) == args.gpu
                assert row["evidence"]["environment"]["executable"]["sha256"]
            for pair in report["comparisons"]:
                assert pair["status"] == "passed" and pair["inverse_mass_equal"]
                assert pair["position_max_abs"] <= pair["tolerance"]["position_abs"]
                assert pair["velocity_max_abs"] <= pair["tolerance"]["velocity_abs"]
                assert pair["last_batch_ratio"] is not None
                if pair["kind"] == "rerun":
                    assert pair["left"]["experiment_id"] == pair["right"]["experiment_id"]
                    assert pair["left"]["run_id"] != pair["right"]["run_id"]
            if extra:
                assert sum(p["kind"] == "rerun" for p in report["comparisons"]) == expected_count
            with (report_dir / "experiments.csv").open(encoding="utf-8-sig", newline="") as stream:
                rows = list(csv.DictReader(stream))
                assert len(rows) == expected_count * (2 if extra else 1)
                assert all(r["gpu_timestamp_ns"] == "" for r in rows)
            assert client.call("simulation.query").value == {"mode": "edit", "run": None}
        assert client.call("scene.query").value == source and client.call("history.status").value == history
    # Offline regeneration is independent of the engine; all existing evidence remains byte-identical.
    before = {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in root.rglob("*") if p.is_file()}
    offline = subprocess.run([sys.executable, "-m", "decker.experiment_report", str(root / "rerun"),
        "--baseline", str(root / "first")], capture_output=True, timeout=60)
    assert offline.returncode == 0, offline.stderr.decode()
    offline_result = json.loads(offline.stdout)
    regenerated = json.loads((Path(offline_result["directory"]) / "report.json").read_bytes())
    assert regenerated["comparison_status"] == "passed" and regenerated["execution_observation"] is None
    assert all(hashlib.sha256(Path(p).read_bytes()).hexdigest() == digest for p, digest in before.items())
    (root / "acceptance.json").write_bytes(canonical({"gpu": args.gpu, "reports": results, "offline": offline_result,
        "source_evidence_unchanged": True, "scene_unchanged": True}))
    print(f"Parameter/seed scan, automatic reports, CPU/GPU tolerances and independent 300-step reruns passed: {root}")


if __name__ == "__main__":
    main()
