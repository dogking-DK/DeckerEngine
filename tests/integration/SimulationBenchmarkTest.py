"""Reject misleading baseline inputs rather than silently aggregating partial results."""
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location("benchmark", Path(__file__).resolve().parents[2] / "scripts/benchmark-simulation.py")
benchmark = importlib.util.module_from_spec(spec)
spec.loader.exec_module(benchmark)


def sample():
    batches = []
    for first in range(0, 300, 8):
        count = min(8, 300 - first)
        phase = "cold" if first == 0 else "tail" if count != 8 else "warmup" if first < 32 else "steady"
        batches.append({"first_step": first, "count": count, "phase": phase, "wall_ns": 100, "gpu": None})
    return {"schema": 1, "status": "passed", "steps": 300, "seed": 42, "dt_ns": 10000000,
            "iterations": 12, "size": 8, "configuration": "RelWithDebInfo", "validation": False,
            "timestamps": False, "batches": batches, "particles": [[0] * 7 for _ in range(64)]}


class Aggregation(unittest.TestCase):
    def read(self, data):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "measurement.json"
            path.write_text(json.dumps(data), encoding="utf-8")
            return benchmark.read_measurement(path)

    def test_nearest_rank_preserves_tail_outlier(self):
        self.assertEqual(benchmark.distribution([1] * 99 + [999]),
                         {"n": 100, "p50": 1, "p95": 1, "p99": 1, "min": 1, "max": 999})
        self.assertEqual(benchmark.distribution([1, 2, 50])["p95"], 50)

    def test_missing_and_nonfinite_are_not_zero(self):
        for values in ([], [None], [float("nan")], [float("inf")], [-1]):
            with self.subTest(values=values), self.assertRaises((ValueError, TypeError)):
                benchmark.distribution(values)

    def test_full_trajectory_has_equal_steady_batches(self):
        result = self.read(sample())
        self.assertEqual(sum(b["count"] for b in result["batches"]), 300)
        self.assertEqual(len([b for b in result["batches"] if b["phase"] == "steady"]), 33)
        self.assertEqual(result["batches"][-1]["phase"], "tail")

    def test_partial_or_noncontiguous_trajectory_rejected(self):
        for mutation in (lambda m: m["batches"].pop(), lambda m: m["batches"][5].update(first_step=32),
                         lambda m: m["batches"][0].update(phase="steady"), lambda m: m.update(status="failed")):
            m = sample()
            mutation(m)
            with self.assertRaises(ValueError):
                self.read(m)

    def test_absent_timestamp_rejected(self):
        m = sample()
        m["timestamps"] = True
        with self.assertRaises(ValueError):
            self.read(m)

    def test_incompatible_measurement_rejected(self):
        for field, value in (("configuration", "Debug"), ("validation", True), ("seed", 1), ("dt_ns", 1),
                             ("iterations", 1), ("steps", 299)):
            m = sample()
            m[field] = value
            with self.subTest(field=field), self.assertRaises(ValueError):
                self.read(m)


if __name__ == "__main__":
    unittest.main()
