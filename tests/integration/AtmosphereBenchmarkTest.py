"""Reject incomplete or misleading performance evidence before aggregation."""
import copy
import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location("benchmark", Path(__file__).resolve().parents[2] / "scripts/benchmark-atmosphere.py")
benchmark = importlib.util.module_from_spec(spec)
spec.loader.exec_module(benchmark)


def measurement():
    profile = {"period_ns": 1, "valid_bits": 64, "submission_ms": 2,
               "zones": [{"name": name, "gpu_ms": 1, "cpu_record_ms": .1}
                         for name in ("aerial perspective volume", "sky and geometry composition")]}
    frames = []
    for mode in ("static", "sky-update", "physical-update", "unlit"):
        for i in range(3 if mode == "physical-update" else 46):
            luts = ({key: copy.deepcopy(profile) for key in ("transmittance", "multiple_scattering", "sky_view")}
                    if mode == "physical-update" or mode == "static" and i == 0 else
                    {"sky_view": copy.deepcopy(profile)} if mode == "sky-update" else {})
            frames.append({"mode": mode, "index": i,
                           "phase": "rebuild" if mode == "physical-update" else "cold" if i == 0 else "warmup" if i < 6 else "steady",
                           **{key: 1 for key in benchmark.CPU_KEYS}, "end_to_end_ms": 5,
                           "lut_builds": luts, "gpu": copy.deepcopy(profile)})
    return {"schema": 1, "status": "passed", "configuration": "RelWithDebInfo", "steady_samples": 40,
            "validation": False, "timestamps": True, "tracy_compiled": False, "tracy_connected": False,
            "preset": {"width": 1920, "height": 1080, "name": "noon"}, "diagnostics": {"errors": 0, "warnings": 0},
            "cpu_heap_live_allocations": 0, "cpu_heap_peak_bytes": 42,
            "memory_device_only": {"all_allocations": 1}, "memory_after_release": {"all_allocations": 1},
            "memory_cache_only": {}, "memory_samples": [{"with_frame": {"all_allocation_bytes": 100}} for _ in frames],
            "memory_inflight_frame": {"all_allocation_bytes": 200},
            "device_queue_ms": 1, "pipeline_create_ms": 1, "atmosphere_create_ms": 1, "frames": frames}


class Aggregation(unittest.TestCase):
    def test_nearest_rank_keeps_tail(self):
        self.assertEqual(benchmark.distribution([1] * 38 + [5, 99])["p95"], 1)
        self.assertEqual(benchmark.distribution([1, 2, 99])["p95"], 99)

    def test_dynamic_sky_is_in_increment_and_cold_is_excluded(self):
        data = measurement()
        data["frames"][0]["gpu"]["submission_ms"] = 999
        summary = benchmark.summarize(data)
        self.assertEqual(summary["modes"]["static"]["incremental_gpu_ms"]["p95"], 0)
        self.assertEqual(summary["modes"]["sky-update"]["incremental_gpu_ms"]["p95"], 2)
        self.assertEqual(summary["modes"]["static"]["frame_gpu_ms"]["n"], 40)
        self.assertEqual(summary["vma_sampled_peak"]["all_allocation_bytes"], 200)

    def test_rejects_incomplete_duplicate_and_mislabeled_frames(self):
        for change in (lambda d: d["frames"].pop(), lambda d: d["frames"][7].update(index=6),
                       lambda d: d["frames"][7].update(phase="warmup")):
            data = measurement(); change(data)
            with self.assertRaises(ValueError): benchmark.summarize(data)

    def test_rejects_missing_or_nonfinite_time(self):
        for value in (None, {"period_ns": 0}, {"period_ns": 1, "valid_bits": 64, "zones": [{"gpu_ms": float("nan"), "cpu_record_ms": 1}], "submission_ms": 1}):
            data = measurement(); data["frames"][6]["gpu"] = value
            with self.assertRaises(ValueError): benchmark.summarize(data)
        data = measurement(); data["frames"][6]["end_to_end_ms"] = float("nan")
        with self.assertRaises(ValueError): benchmark.summarize(data)

    def test_rejects_missing_sky_build_and_pass(self):
        data = measurement(); data["frames"][46]["lut_builds"] = {}
        with self.assertRaises(ValueError): benchmark.summarize(data)
        data = measurement(); data["frames"][6]["gpu"]["zones"].pop()
        with self.assertRaises(ValueError): benchmark.summarize(data)

    def test_rejects_failed_validation_debug_and_leak(self):
        for key, value in (("status", "failed"), ("validation", True), ("configuration", "Debug"), ("cpu_heap_live_allocations", 1)):
            data = measurement(); data[key] = value
            with self.assertRaises(ValueError): benchmark.summarize(data)


if __name__ == "__main__":
    unittest.main()
