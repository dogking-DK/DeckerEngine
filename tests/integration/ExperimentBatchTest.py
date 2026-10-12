"""M13.2: one real CLI launch, serialized experiments, durable progress and bound artifacts."""

import argparse
import json
from pathlib import Path
import subprocess
import sys
import time
from uuid import uuid4

from decker.experiments import canonical, expand_plan
from decker.experiment_index import verify_index
from decker.experiment_batch import ExperimentBatchError, _runtime_lease, read_progress, run_plan
from PythonClientTest import server


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("runner", "client", "work-dir"):
        parser.add_argument("--" + name, required=True)
    parser.add_argument("--gpu", action="store_true")
    parser.add_argument("--cpu-only", action="store_true", help="Require a host built without RenderSimulation")
    args = parser.parse_args()
    args.pipe_timeout_ms = 60000
    root = Path(args.work_dir).resolve() / uuid4().hex
    root.mkdir(parents=True)
    plan = {"format": "DeckerExperimentPlan", "version": 1,
        "base": {"steps": 9, "fixed_dt_ns": 10000000,
                 "cloth": {"columns": 4 if args.gpu else 17, "rows": 4 if args.gpu else 16, "iterations": 2}},
        "sweep": {"steps": [0, 9]}, "seeds": [42, 43],
        "solvers": ["xpbd_gpu", "xpbd_cpu"] if args.gpu else ["xpbd_cpu"],
        "output": {"image": args.gpu, "width": 96, "height": 72},
        "budget": {"max_experiments": 8, "max_total_steps": 1000000, "max_work_units": 10**10}}
    (root / "plan.json").write_bytes(canonical(plan))
    endpoint = "dk-batch-" + uuid4().hex
    command = [sys.executable, "-m", "decker.experiment_batch", "run", str(root / "plan.json"),
               "--pipe", endpoint, "--ctl", args.client, "--project-root", str(root),
               "--runner", args.runner, "--poll-interval", ".01", "--experiment-timeout", "90"]
    with server(args, root, endpoint) as client:
        client.timeout = 60
        source = client.call("scene.new", {"name": "批处理 M13.2"}).value
        before = client.call("scene.query").value
        history = client.call("history.status").value
        caps = client.call("runtime.capabilities").value
        if args.cpu_only:
            assert caps["simulation"]["gpu"] is False and caps["simulation"]["experiment_export"] is False
        # A different process and different output directory must still respect the same Runtime lease.
        with _runtime_lease(client.hello()["session"]):
            blocked = subprocess.run([*command, "--output", "contender"], capture_output=True, text=True, encoding="utf-8", timeout=15)
        assert blocked.returncode == 2 and "already has a batch executor" in blocked.stderr, blocked
        assert not (root / "contender").exists()
        assert client.call("simulation.query").value == {"mode": "edit", "run": None}
        snapshots = []
        with (root / "cli.stdout").open("wb") as output, (root / "progress.jsonl").open("wb") as progress:
            process = subprocess.Popen([*command, "--output", "batch"], stdout=output, stderr=progress,
                                       creationflags=subprocess.CREATE_NO_WINDOW)
            try:
                deadline = time.monotonic() + 180
                while process.poll() is None:
                    assert time.monotonic() < deadline, "Batch CLI timed out"
                    if (root / "batch/batch.json").exists():
                        current = read_progress(root / "batch")
                        snapshots.append({"sequence": current["sequence"], "status": current["status"],
                                          "states": [e["status"] for e in current["experiments"]]})
                    time.sleep(.03)
                assert process.returncode == 0, (root / "progress.jsonl").read_text(encoding="utf-8")
            finally:
                if process.poll() is None:
                    process.kill()
                    process.wait(timeout=10)
        result = json.loads((root / "cli.stdout").read_bytes())
        report = read_progress(root / "batch")
        index = verify_index(root / "batch")
        expanded = expand_plan(plan)
        assert result["status"] == report["status"] == "succeeded"
        assert result["counts"]["succeeded"] == len(expanded["experiments"])
        assert report["source"] == source and report["plan_id"] == index["plan_id"] == expanded["plan_id"]
        assert snapshots and any(s["status"] == "running" for s in snapshots)
        events = [json.loads(line) for line in (root / "progress.jsonl").read_text(encoding="utf-8").splitlines()]
        assert [e["sequence"] for e in events] == sorted({e["sequence"] for e in events})
        event_positions = {}
        for ordinal, event in enumerate(events):
            if event["current"]:
                item = event["current"]
                event_positions.setdefault((item["experiment_id"], item["status"]), ordinal)
        ids = [e["experiment_id"] for e in report["experiments"]]
        assert all(event_positions[(previous, "succeeded")] < event_positions[(following, "submitting")]
                   for previous, following in zip(ids, ids[1:]))
        assert len({e["run_id"] for e in report["experiments"]}) == len(ids)
        for item, entry in zip(report["experiments"], index["experiments"]):
            attempt = entry["attempts"][0]
            assert item["status"] == "succeeded" and item["artifact_attempt_id"] == attempt["attempt_id"]
            assert item["completed_steps"] == item["target_steps"] == entry["config"]["steps"]
            assert attempt["source"]["run_id"] == item["run_id"]
            assert attempt["source"]["document_id"] == source["document_id"] and attempt["source"]["revision"] == source["revision"]
            assert item["submission"]["ticket"]["session"] == report["session"]
            assert set(attempt["files"]) == ({"config.json", "metrics.json", "particles.json", "provenance.json"}
                                             | ({"image.ppm"} if args.gpu else set()))
            if not item["target_steps"]:
                assert item["submission"]["params"]["count"] == 1 and item["submission"]["params"]["paused"] is True
            else:
                assert attempt["source"]["performance"]["scope"] == "batch_submit_to_completion"
        status = subprocess.run([sys.executable, "-m", "decker.experiment_batch", "status", str(root / "batch")],
                                capture_output=True, text=True, encoding="utf-8", timeout=15)
        assert status.returncode == 0 and json.loads(status.stdout) == report, status.stderr
        saved = (root / "batch/index.json").read_bytes()
        repeated = subprocess.run([*command, "--output", "batch"], capture_output=True, text=True, encoding="utf-8", timeout=15)
        assert repeated.returncode == 2 and (root / "batch/index.json").read_bytes() == saved
        assert client.call("simulation.query").value == {"mode": "edit", "run": None}
        assert client.call("scene.query").value == before and client.call("history.status").value == history

        # Real external cancellation has its own terminal state; no output or later experiment is fabricated.
        cancellation = json.loads(json.dumps(plan))
        cancellation.update(solvers=["xpbd_cpu"], sweep={})
        cancellation["base"].update(steps=1000000, cloth={"columns": 4, "rows": 4, "iterations": 2})
        cancellation["output"]["image"] = False
        cancellation["budget"].update(max_total_steps=2000000, max_work_units=10**10)
        cancelled_run = None
        def cancel(event):
            nonlocal cancelled_run
            item = event["current"]
            if item and item["status"] == "running" and cancelled_run is None:
                cancelled_run = item["run_id"]
                client.call("simulation.cancel", {"run_id": cancelled_run})
        try:
            run_plan(client, cancellation, project_root=root, output="cancelled", progress=cancel, experiment_timeout=30)
            raise AssertionError("Cancelled batch reported success")
        except ExperimentBatchError as error:
            assert error.progress["status"] == "cancelled", error
        cancelled = read_progress(root / "cancelled")
        assert [e["status"] for e in cancelled["experiments"]] == ["cancelled", "pending"]
        assert all(not e["attempts"] for e in verify_index(root / "cancelled")["experiments"])
        client.call("simulation.stop", {"run_id": cancelled_run})
        deadline = time.monotonic() + 10
        while client.call("simulation.query").value["mode"] != "edit":
            assert time.monotonic() < deadline
            time.sleep(.01)
        assert client.call("scene.query").value == before and client.call("history.status").value == history
        (root / "snapshots.json").write_bytes(canonical(snapshots))
        (root / "report.json").write_bytes(canonical({"gpu": args.gpu, "runtime": caps, "summary": result,
            "live_snapshots": len(snapshots), "cross_process_lease": True, "cancelled_tail_pending": True}))
    print(f"One CLI batch, serial scheduling, live progress, bound artifacts, lease and cancellation passed: {root}")


if __name__ == "__main__":
    main()
