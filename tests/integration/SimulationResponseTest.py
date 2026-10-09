"""Aggregation must reject incomplete, failed or misleading response measurements."""
import importlib.util
import json
from pathlib import Path
import tempfile
from types import SimpleNamespace
import unittest

spec = importlib.util.spec_from_file_location("response", Path(__file__).resolve().parents[2] / "scripts/benchmark-simulation-response.py")
response = importlib.util.module_from_spec(spec)
spec.loader.exec_module(response)


class Aggregation(unittest.TestCase):
    def fixture(self, root):
        manifest = {"status": "passed", "repetitions": 1, "hot_queries": 5, "hosts": ["runner"],
                    "backends": ["gpu"], "sizes": [8], "cases": [], "process_per_action": True}
        for phase in response.PHASES:
            name = "r0-runner-gpu-8-" + phase
            manifest["cases"].append(name)
            directory = root / name
            directory.mkdir()
            samples = []
            for action in response.ACTIONS:
                for method in ("simulation.run", "simulation.query", "runtime.shutdown" if action == "shutdown" else "simulation." + action):
                    samples.append({"method": method, "action": action, "status": "passed", "latency_ns": 1,
                                    "boundary_ns": 2})
                if phase == "steady" and action == "pause":
                    samples.extend({"method": "simulation.query", "action": action, "status": "passed", "latency_ns": 1,
                                    "hot_sample": True} for _ in range(5))
            response.save(directory / "control.json", {"status": "passed", "host": "runner", "backend": "gpu", "size": 8,
                                                        "phase": phase, "repetition": 0, "samples": samples})
        response.save(root / "manifest.json", manifest)
        return manifest

    def test_smoke_is_partial_not_full_acceptance(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            self.fixture(root)
            self.assertEqual(response.summarize(root)["status"], "partial")

    def test_selected_action_recheck_is_not_full_acceptance(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            manifest = self.fixture(root)
            manifest["actions"] = ["shutdown"]
            for name in manifest["cases"]:
                path = root / name / "control.json"
                case = json.loads(path.read_text())
                case["samples"] = [s for s in case["samples"] if s["action"] == "shutdown"]
                response.save(path, case)
            response.save(root / "manifest.json", manifest)
            self.assertEqual(response.summarize(root)["status"], "partial")

    def test_missing_duplicate_failed_and_empty_trials_are_rejected(self):
        for mode in ("missing", "duplicate", "failed", "empty", "warm-process"):
            with self.subTest(mode=mode), tempfile.TemporaryDirectory() as temp:
                root = Path(temp)
                manifest = self.fixture(root)
                if mode == "missing":
                    manifest["cases"].pop()
                elif mode == "duplicate":
                    manifest["cases"][-1] = manifest["cases"][0]
                elif mode == "failed":
                    manifest["status"] = "failed"
                elif mode == "warm-process":
                    manifest["process_per_action"] = False
                else:
                    p = root / manifest["cases"][0] / "control.json"
                    case = json.loads(p.read_text())
                    case["samples"] = []
                    response.save(p, case)
                response.save(root / "manifest.json", manifest)
                with self.assertRaises(ValueError):
                    response.summarize(root)
                self.assertEqual(json.loads((root / "summary.json").read_text())["status"], "failed")

    def test_timeout_or_tail_outlier_cannot_be_reported_as_passed(self):
        for mode in ("timeout", "tail"):
            with self.subTest(mode=mode), tempfile.TemporaryDirectory() as temp:
                root = Path(temp)
                manifest = self.fixture(root)
                p = root / manifest["cases"][0] / "control.json"
                case = json.loads(p.read_text())
                if mode == "timeout":
                    case["samples"][0]["status"] = "failed"
                else:
                    case["samples"][0]["latency_ns"] = 300000000
                response.save(p, case)
                with self.assertRaises(ValueError):
                    response.summarize(root)

    def test_independent_controls_are_counted_across_phases(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            manifest = self.fixture(root)
            originals = list(manifest["cases"])
            for rep in range(1, 7):
                for name in originals:
                    case = json.loads((root / name / "control.json").read_text())
                    case["repetition"] = rep
                    target = name.replace("r0-", f"r{rep}-", 1)
                    (root / target).mkdir()
                    response.save(root / target / "control.json", case)
                    manifest["cases"].append(target)
            # Seven rounds give 21 independent controls, but only 35 hot queries: still partial.
            manifest["repetitions"] = 7
            manifest["cases"] = manifest["cases"][:21]
            response.save(root / "manifest.json", manifest)
            self.assertEqual(response.summarize(root)["status"], "partial")
            # A focused plan reuses seven rounds and adds only the missing hot queries.
            manifest["trial_plan"] = {n: 5 if n.endswith("steady") else 0 for n in manifest["cases"]}
            name = originals[-1]
            case = json.loads((root / name / "control.json").read_text())
            hot = next(s for s in case["samples"] if s.get("hot_sample"))
            case["samples"].extend(dict(hot) for _ in range(65))
            response.save(root / name / "control.json", case)
            manifest["trial_plan"][name] += 65
            response.save(root / "manifest.json", manifest)
            result = response.summarize(root)
            self.assertEqual(result["status"], "passed")
            self.assertEqual(result["hot_query_count"]["runner/gpu/8"], 100)

    def test_only_full_exit_p95_is_relaxed_to_400ms(self):
        for method, field, milliseconds, accepted in (
                ("runtime.shutdown", "boundary_ns", 400, True),
                ("runtime.shutdown", "boundary_ns", 401, False),
                ("runtime.shutdown", "boundary_ns", 1001, False),
                ("runtime.shutdown", "latency_ns", 251, False),
                ("simulation.stop", "boundary_ns", 251, False),
                ("simulation.cancel", "boundary_ns", 251, False),
                ("simulation.query", "latency_ns", 101, False)):
            with self.subTest(method=method, field=field, milliseconds=milliseconds), tempfile.TemporaryDirectory() as temp:
                root = Path(temp)
                manifest = self.fixture(root)
                originals = list(manifest["cases"])
                manifest.update(repetitions=7, cases=[])
                for rep in range(7):
                    for name in originals:
                        case = json.loads((root / name / "control.json").read_text())
                        case["repetition"] = rep
                        for sample in case["samples"]:
                            if sample["method"] == method:
                                sample[field] = milliseconds * 1000000
                        target = name.replace("r0-", f"r{rep}-", 1)
                        (root / target).mkdir(exist_ok=True)
                        response.save(root / target / "control.json", case)
                        manifest["cases"].append(target)
                response.save(root / "manifest.json", manifest)
                if accepted:
                    self.assertEqual(response.summarize(root)["status"], "partial")
                else:
                    with self.assertRaises(ValueError):
                        response.summarize(root)

    def test_continuation_preserves_every_completed_trial_and_detects_tampering(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            manifest = self.fixture(root)
            manifest.update(binaries={}, gui_validation="required", simulation_validation="if_available", status="failed")
            name = manifest["cases"][-1]
            case = json.loads((root / name / "control.json").read_text())
            case["status"] = "failed"
            response.save(root / name / "control.json", case)
            response.save(root / "manifest.json", manifest)
            target = {k: manifest[k] for k in ("hosts", "backends", "sizes", "binaries", "gui_validation",
                                               "simulation_validation", "process_per_action")}
            args = SimpleNamespace(complete_from=root, interrupted_case=name, interruption_reason="Manually stopped fixture",
                                   hosts=["runner"], backends=["gpu"], sizes=[8])
            plan = response.continuation(args, target)
            self.assertEqual(len(plan), 20)
            self.assertEqual(set(target["imports"]), set(manifest["cases"][:-1]))
            self.assertEqual(sum(plan.values()), 100)
            imported = manifest["cases"][0]
            response.read_case(root, target, imported)
            (root / imported / "control.json").write_text("{}")
            with self.assertRaises(ValueError):
                response.read_case(root, target, imported)


if __name__ == "__main__":
    unittest.main()
