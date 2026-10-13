"""Reject incomplete, duplicated or mislabeled cloud performance observations."""
import copy
import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location("benchmark", Path(__file__).resolve().parents[2]/"scripts/benchmark-clouds.py")
b = importlib.util.module_from_spec(spec)
spec.loader.exec_module(b)


def measurement():
    counter = 0
    def profile(names, milliseconds=2):
        nonlocal counter
        counter += 1
        return dict(submission_id=counter, period_ns=1, valid_bits=64, submission_ms=milliseconds,
                    zones=[dict(name=n, gpu_ms=100, cpu_record_ms=.1) for n in names])
    rows = []
    for mode in b.MODES:
        combined, air, motion = mode.startswith("combined"), not mode.startswith("unlit"), mode.endswith("motion")
        warmup = 32 if combined else 5
        for i in range(warmup+21):
            step = max(0, i-warmup) if motion else 0
            lut_names = ({"transmittance", "multiple_scattering", "sky_view"} if mode == "combined-static" and i == 0 else
                         {"sky_view"} if air and (motion and step > 0 or mode == "air-static" and i == 0) else set())
            shadow = combined and (mode == "combined-static" and i == 0 or motion and step > 0)
            names = ({"half resolution cloud transport", "depth preserving cloud reconstruction"} if combined else
                     {"aerial perspective volume", "sky and geometry composition"} if air else {"opaque"})
            rows.append(dict(mode=mode, index=i, logical_index=i-warmup-1, phase="cold" if i == 0 else "warmup" if i <= warmup else "steady",
                             eye_m=[step*.25, 100, 0], yaw_rad=step*.0005, time_seconds=step/30,
                             **{k: 1 for k in b.CPU_KEYS[:-1]}, end_to_end_ms=5,
                             lut_builds={n: profile([n]) for n in lut_names}, shadow_updated=shadow,
                             shadow_gpu=profile(["cloud shadow column integration"], 3) if shadow else None,
                             shadow_generation=int(combined), history=dict(available=combined and i > 0), gpu=profile(names)))
    return dict(schema=1, status="passed", kind="performance", validation=False, smoke=False, timestamps=True,
                configuration="RelWithDebInfo", steady_samples=20, tracy_compiled=False, tracy_connected=False,
                preset=dict(name="sparse", width=1920, height=1080, quality="cloud_temporal_v1", warmup=32,
                            shadow=dict(center_m=[0, 0, 0], extent_m=8192, resolution=256, steps=256),
                            environment=dict(clouds=dict(enabled=True, seed=1729, generator_version=1, weather_resolution=128, base_resolution=128, detail_resolution=32))),
                diagnostics=dict(errors=0, warnings=0), cpu_heap_live_allocations=0, cpu_heap_peak_bytes=123,
                memory_device_only=dict(all_allocations=1), memory_after_release=dict(all_allocations=1),
                memory_geometry_only=dict(device_local_allocation_bytes=100),
                memory_samples=[dict(mode=m, inflight_with_old_frame=dict(device_local_allocation_bytes=900), steady_cache_history=dict(device_local_allocation_bytes=400)) for m in b.MODES],
                device_queue_ms=1, program_geometry_create_ms=1, cpu_field_ms=1, texture_upload_wall_ms=1, frames=rows)


class Evidence(unittest.TestCase):
    def test_increment_counts_shadow_and_luts_once_without_adding_child_zones(self):
        result = b.summarize(measurement())
        self.assertEqual(result["increments"]["static"]["cloud_gpu_ms"]["p95"], 0)
        self.assertEqual(result["increments"]["motion"]["cloud_gpu_ms"]["p95"], 3)
        self.assertEqual(result["increments"]["motion"]["combined_gpu_ms"]["p95"], 5)
        self.assertEqual(result["environment_steady_bytes"], 300)
        self.assertEqual(result["environment_sampled_peak_bytes"], 800)
        self.assertEqual(result["modes"]["combined-motion"]["total_gpu_ms"]["n"], 20)

    def test_missing_duplicate_misclassified_and_changed_inputs_reject(self):
        for change in (lambda d: d["frames"].pop(), lambda d: d["frames"][34].update(index=33),
                       lambda d: d["frames"][34].update(phase="warmup"), lambda d: d["frames"][87].update(time_seconds=99),
                       lambda d: d["preset"]["shadow"].update(steps=64)):
            data = measurement(); change(data)
            with self.assertRaises(ValueError): b.summarize(data)

    def test_missing_measurements_and_double_counted_submission_reject(self):
        for change in (lambda r: r.update(gpu=None), lambda r: r.update(shadow_gpu=None),
                       lambda r: r.update(lut_builds={}), lambda r: r["gpu"].update(submission_ms=float("nan")),
                       lambda r: r["shadow_gpu"].update(submission_id=r["gpu"]["submission_id"]),
                       lambda r: r["gpu"].update(zones=[dict(name="wrong", gpu_ms=1, cpu_record_ms=1)])):
            data = measurement(); change(data["frames"][86])
            with self.assertRaises(ValueError): b.summarize(data)

    def test_failed_debug_validation_leak_and_missing_inflight_reject(self):
        for key, value in (("status", "failed"), ("configuration", "Debug"), ("validation", True), ("cpu_heap_live_allocations", 1), ("smoke", True), ("memory_samples", [])):
            data = measurement(); data[key] = value
            with self.assertRaises(ValueError): b.summarize(data)

    def test_off_has_no_fabricated_gpu_values(self):
        data = measurement(); data["timestamps"] = False
        for r in data["frames"]:
            r["gpu"] = r["shadow_gpu"] = None
            r["lut_builds"] = {key: None for key in r["lut_builds"]}
        result = b.summarize(data)
        self.assertEqual(result["increments"], {})
        self.assertNotIn("total_gpu_ms", result["modes"]["combined-motion"])

    def test_nearest_rank_and_incomplete_rounds(self):
        self.assertEqual(b.distribution(list(range(1, 21)))["p95"], 19)
        with self.assertRaises(ValueError): b.matrix_candidates([])


if __name__ == "__main__":
    unittest.main()
