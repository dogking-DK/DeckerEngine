"""Report provenance, pairing, numerical gates and publication failure behavior."""

import copy
import csv
import io
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

from decker._experiment_lock import exclusive_path
from decker.experiments import ExperimentError, canonical
from decker.experiment_batch import ExperimentBatchError, main, read_progress, resume_plan, run_plan
from decker.experiment_report import DEFAULT_TOLERANCES, tolerance_policy, write_report
from test_experiment_batch import FakeClient
from test_experiments import plan


class ReportTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)

    def batch(self, name, value=None, delta=0, velocity=0, mass=1, fail=False, no_timing=False, timing=None):
        client = FakeClient()
        def hook(method, result):
            if method == "runtime.capabilities":
                result["simulation"]["gpu"] = True
            if method == "simulation.particles":
                for particle in result["particles"]:
                    particle["position"][0] = client.run["cloth"]["seed"] + delta
                    particle["velocity"][0] = velocity
                    particle["inverse_mass"] = mass
            if method == "simulation.query" and no_timing and result["run"]:
                result["run"]["task"]["performance"] = None
            elif method == "simulation.query" and timing is not None and result["run"]:
                result["run"]["task"]["performance"] = timing
            if method == "simulation.sample" and fail:
                raise RuntimeError("<script>alert(1)</script>")
        client.hook = hook
        try:
            run_plan(client, value or plan(), project_root=self.root, output=name)
        except ExperimentBatchError:
            if not fail:
                raise
        return self.root / name

    def report(self, directory, **kwargs):
        result = write_report(directory, **kwargs)
        return result, json.loads((Path(result["directory"]) / "report.json").read_bytes())

    def test_reordered_scan_pairs_by_identity_and_preserves_all_evidence(self):
        p = plan()
        p.update(seeds=[42, 43], solvers=["xpbd_gpu", "xpbd_cpu"], sweep={"cloth.iterations": [2, 4]})
        old = self.batch("old", p)
        p["seeds"].reverse()
        p["sweep"]["cloth.iterations"].reverse()
        new = self.batch("new", p)
        result, report = self.report(new, baseline=old)
        self.assertEqual(report["comparison_status"], "passed")
        self.assertEqual(len(report["comparisons"]), 12)
        self.assertNotEqual(report["current"]["plan_id"], report["baseline"]["plan_id"])
        for pair in report["comparisons"]:
            self.assertEqual(pair["position_max_abs"], 0)
            self.assertEqual(pair["last_batch_ratio"], 1)
            self.assertNotEqual(pair["left"]["run_id"], pair["right"]["run_id"])
        row = report["current"]["experiments"][0]
        self.assertEqual(row["evidence"]["files"]["metrics.json"]["sha256"],
                         row["registered_attempts"][0]["files"]["metrics.json"]["sha256"])
        self.assertIsNone(row["gpu_timestamp_ns"])
        self.assertIsNone(report["execution_observation"])
        folder = Path(result["directory"])
        self.assertEqual({p.name for p in folder.iterdir()}, {"report.json", "report.html", "experiments.csv", "comparisons.csv"})
        csv_rows = list(csv.DictReader(io.StringIO((folder / "experiments.csv").read_text(encoding="utf-8-sig"))))
        self.assertEqual(len(csv_rows), 16)
        self.assertEqual(csv_rows[0]["gpu_timestamp_ns"], "")

    def test_tolerance_boundary_failure_and_inverse_mass(self):
        baseline = self.batch("baseline")
        policy = copy.deepcopy(DEFAULT_TOLERANCES)
        policy["same_solver"] = {"position_abs": .125, "velocity_abs": .25}
        for name, delta, velocity, mass, status in (("edge", .125, .25, 1, "passed"),
                ("position", .25, 0, 1, "failed"), ("velocity", 0, .5, 1, "failed"),
                ("mass", 0, 0, 0, "failed")):
            with self.subTest(name=name):
                _, report = self.report(self.batch(name, delta=delta, velocity=velocity, mass=mass),
                                        baseline=baseline, tolerances=policy)
                self.assertEqual(report["comparison_status"], status)
                self.assertEqual(report["comparisons"][0]["tolerance"], policy["same_solver"])

    def test_missing_counterparts_use_union_and_no_false_pass(self):
        p = plan()
        baseline = self.batch("baseline", p)
        p["seeds"] = [43]
        _, report = self.report(self.batch("current", p), baseline=baseline)
        self.assertEqual(report["comparison_status"], "incomplete")
        self.assertEqual(len(report["comparisons"]), 2)
        self.assertTrue(all(p["position_max_abs"] is None for p in report["comparisons"]))

    def test_failed_pending_rows_stay_null_and_html_escapes_errors(self):
        p = plan()
        p["seeds"] = [42, 43]
        result, report = self.report(self.batch("failed", p, fail=True))
        self.assertEqual(report["comparison_status"], "incomplete")
        self.assertEqual([r["status"] for r in report["current"]["experiments"]], ["failed", "pending"])
        self.assertTrue(all(r["metrics"] is None and r["last_batch"] is None for r in report["current"]["experiments"]))
        html = (Path(result["directory"]) / "report.html").read_text(encoding="utf-8")
        self.assertNotIn("<script>", html)
        self.assertIn("&lt;script&gt;", html)

    def test_failed_collected_attempt_is_not_used_as_success(self):
        client = FakeClient()
        def fail(event):
            if event["current"] and event["current"]["status"] == "stopping":
                raise RuntimeError("after indexing")
        with self.assertRaises(ExperimentBatchError):
            run_plan(client, plan(), project_root=self.root, output="collected", progress=fail)
        _, report = self.report(self.root / "collected")
        row = report["current"]["experiments"][0]
        self.assertTrue(row["evidence"] and row["registered_attempts"])
        self.assertEqual(row["status"], "failed")
        self.assertIsNone(row["metrics"])

    def test_zero_steps_and_unobserved_timing_stay_missing(self):
        p = plan()
        p["base"]["steps"] = 0
        old = self.batch("zero1", p)
        _, report = self.report(self.batch("zero2", p), baseline=old)
        self.assertEqual(report["comparison_status"], "passed")
        self.assertIsNone(report["comparisons"][0]["last_batch_ratio"])
        self.assertIsNone(report["current"]["experiments"][0]["last_batch"])
        _, report = self.report(self.batch("no-clock", no_timing=True))
        self.assertIsNone(report["current"]["experiments"][0]["last_batch"])
        self.assertEqual(report["comparison_status"], "not_compared")

    def test_tampered_or_missing_registered_artifact_refuses_publication(self):
        for mode in ("missing", "tampered"):
            root = self.batch(mode)
            particle = next(root.glob("results/*/particles.json"))
            if mode == "missing":
                particle.unlink()
            else:
                particle.write_bytes(particle.read_bytes() + b" ")
            with self.assertRaises(ExperimentError):
                write_report(root)
            self.assertFalse((root / "reports").exists())

    def test_unequal_batch_samples_and_zero_denominator_do_not_create_ratios(self):
        baseline = self.batch("baseline", timing={"steps": 3, "batch_steps": 3, "wall_time_ns": 0})
        _, zero = self.report(self.batch("normal"), baseline=baseline)
        self.assertEqual(zero["comparisons"][0]["last_batch_delta_ns"], 1000)
        self.assertIsNone(zero["comparisons"][0]["last_batch_ratio"])
        _, unequal = self.report(self.batch("partial", timing={"steps": 2, "batch_steps": 2, "wall_time_ns": 900}),
                                 baseline=baseline)
        self.assertIsNone(unequal["comparisons"][0]["last_batch_delta_ns"])
        self.assertIsNone(unequal["comparisons"][0]["last_batch_ratio"])
        self.assertEqual(unequal["comparison_status"], "passed")

    def test_successful_retry_selects_current_attempt_and_retains_failed_history(self):
        client = FakeClient()
        def fail(event):
            if event["current"] and event["current"]["status"] == "stopping":
                raise RuntimeError("after indexing")
        with self.assertRaises(ExperimentBatchError):
            run_plan(client, plan(), project_root=self.root, output="retry", progress=fail)
        previous = read_progress(self.root / "retry")["experiments"][0]
        resume_plan(client, project_root=self.root, output="retry", retry_failed=True)
        _, report = self.report(self.root / "retry")
        row = report["current"]["experiments"][0]
        self.assertEqual(row["status"], "succeeded")
        self.assertEqual(row["history"][0]["run_id"], previous["run_id"])
        self.assertEqual(row["history"][0]["status"], "failed")
        self.assertEqual(len(row["registered_attempts"]), 2)
        self.assertNotEqual(row["run_id"], previous["run_id"])
        self.assertEqual(row["evidence"]["source"]["run_id"], row["run_id"])

    def test_atomic_publication_failure_preserves_old_reports_and_inputs(self):
        root = self.batch("source")
        result, _ = self.report(root)
        before = {p: p.read_bytes() for p in root.rglob("*") if p.is_file()}
        with patch.object(Path, "rename", side_effect=OSError("report publish failure")):
            with self.assertRaises(ExperimentError):
                write_report(root)
        self.assertEqual(before, {p: p.read_bytes() for p in root.rglob("*") if p.is_file()})
        self.assertEqual(list((root / "reports").iterdir()), [Path(result["directory"])])

    def test_active_writer_and_self_comparison_are_rejected(self):
        root = self.batch("source")
        with exclusive_path("batch", root), self.assertRaises(ExperimentError):
            write_report(root)
        with self.assertRaises(ExperimentError):
            write_report(root, baseline=root)
        shutil.copytree(root, self.root / "copy")
        with self.assertRaises(ExperimentError):
            write_report(root, baseline=self.root / "copy")

    def test_invalid_policies_have_no_side_effects(self):
        root = self.batch("source")
        for number in (-1, True, float("nan"), float("inf")):
            policy = copy.deepcopy(DEFAULT_TOLERANCES)
            policy["same_solver"]["position_abs"] = number
            with self.assertRaises(ExperimentError):
                write_report(root, tolerances=policy)
        with self.assertRaises(ExperimentError):
            tolerance_policy({**DEFAULT_TOLERANCES, "version": True})
        self.assertFalse((root / "reports").exists())

    def test_offline_cli_numerical_failure_returns_nonzero_with_report(self):
        baseline = self.batch("baseline")
        current = self.batch("different", delta=1)
        result = subprocess.run([sys.executable, "-m", "decker.experiment_report", str(current),
            "--baseline", str(baseline)], capture_output=True, text=True, encoding="utf-8", timeout=15)
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertEqual(json.loads(result.stdout)["comparison_status"], "failed")

    def test_cli_report_failure_preserves_successful_batch_and_does_not_rerun(self):
        client = FakeClient()
        path = self.root / "plan.json"
        path.write_bytes(canonical(plan()))
        argv = ["batch", "run", str(path), "--pipe", client.endpoint, "--project-root", str(self.root), "--output", "cli"]
        with patch.object(sys, "argv", argv), patch("decker.experiment_batch.Client", return_value=client), \
             patch("decker.experiment_report.write_report", side_effect=ExperimentError("report IO")), \
             patch("sys.stdout", new_callable=io.StringIO) as output, patch("sys.stderr", new_callable=io.StringIO):
            with self.assertRaises(SystemExit) as error:
                main()
        self.assertEqual(error.exception.code, 2)
        self.assertEqual(json.loads(output.getvalue())["report_error"], "report IO")
        self.assertEqual(read_progress(self.root / "cli")["status"], "succeeded")
        self.assertEqual(sum(method == "simulation.run" for method, _ in client.calls), 1)


if __name__ == "__main__":
    unittest.main()
