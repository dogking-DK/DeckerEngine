"""Adversarial evidence checks: incomplete data must never become a passing report."""
import copy
import importlib.util
from pathlib import Path
import unittest

path = Path(__file__).resolve().parents[2] / "scripts/benchmark-voxels.py"
spec = importlib.util.spec_from_file_location("voxel_benchmark", path)
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


def fixture():
    mem = dict(allocations=0, allocation_bytes=0, block_bytes=0, device_local_bytes=0, host_visible_bytes=0)
    gpu = dict(submission_ms=1., valid_bits=64, period_ns=1., zones=[dict(name="submission", ms=1.)])
    query = dict(gpu=gpu, rays=1024, visited=16, skipped=2)
    runs = []
    for round_id in range(3):
        d = dict(schema=1, kind="voxel-raster-baseline", matrix_version=1, status="passed", configuration="Release", compiler="fixture",
                 validation=False, smoke=False, round=round_id, steady_samples=8, warmup=2, tracy_connected=False,
                 diagnostics=dict(errors=0, warnings=0), heap_live_allocations=0, after_release=mem, device_only=mem,
                 device=dict(name="fixture", driver="fixture"), shader_fingerprint="fixture", query_reflection={}, cases=[])
        for name in sorted(module.CASES):
            c = dict(name=name, domain_min=[0, 0, 0], domain_max=[63, 63, 63], dense_reference_bytes=4,
                     source_build_ms=1., conversion_ms=1.,
                     storage=dict(source_tree_bytes=1, normalized_tree_bytes=1, nano_bytes=1, conversion_live_payload_bytes=3,
                                  nodes=[0, 0, 0], tiles=[1, 0, 0], occupancy=1., aabb_as_sbt=None),
                     conversion_process_memory=dict(samples=2, interval_ms=1, internal_scratch_exact=False, sampled_working_peak=1,
                                                    working_before=1, sampled_private_peak=1, private_before=1, process_lifetime_working_peak=1, process_lifetime_commit_peak=1),
                     upload_pending=dict(mem), old_generation_and_upload=dict(mem), before_case=dict(mem), after_release=dict(mem), upload=dict(gpu=gpu,cpu_submit_ms=1.,wait_ms=1.),
                     queries=dict(oracle_rays=1024, oracle="analytic_boxes" if name in ("islands", "root_tiles") else "dense_dda",
                                  samples=[dict(phase="warmup" if i < 2 else "steady", tuned=query, baseline=query) for i in range(10)],
                                  ablation=dict(maximum_t=4096, hdda=dict(query, rays=32), cell_step=dict(query, rays=32)), checksum=1), renders=[])
            for w, h in [(640, 480), (1280, 720)]:
                for projection in ("perspective", "orthographic"):
                    c["renders"].append(dict(width=w, height=h, projection=projection, checksum=1,
                        quality=dict(pixels=1024, depth_pixels=1024, depth_extent=[256, 256*h//w], maximum_color_error_bytes=0, maximum_depth_error=0),
                        old_and_pending_frame=dict(mem, allocation_bytes=1), after_frames=mem,
                        frames=[dict(phase="cold" if i == 0 else "warmup" if i < 3 else "steady", gpu=gpu,
                                     cpu_submit_ms=1., wait_ms=1., readback_ms=1., end_to_end_ms=3.) for i in range(11)]))
            d["cases"].append(c)
        runs.append(copy.deepcopy(d))
    return runs


class EvidenceTests(unittest.TestCase):
    def test_complete(self):
        self.assertEqual(module.aggregate(fixture())["status"], "passed")

    def test_preconditioning(self):
        runs = fixture()
        for run in runs:
            run.update(measurement_version=2, device_precondition_frames=64,
                       precondition={"frames": [copy.deepcopy(run["cases"][0]["renders"][0]["frames"][0]) for _ in range(64)]})
        self.assertEqual(module.aggregate(runs)["precondition_wall_ms"], [192., 192., 192.])
        runs[1]["precondition"]["frames"].pop()
        with self.assertRaises(ValueError):
            module.aggregate(runs)

    def test_bad_evidence(self):
        mutations = [
            lambda r: r.pop(),
            lambda r: r[1].update(round=0),
            lambda r: r[0].update(status="failed"),
            lambda r: r[0].update(configuration="Debug"),
            lambda r: r[0].update(validation=True),
            lambda r: r[0].update(shader_fingerprint="different"),
            lambda r: r[0]["cases"].pop(),
            lambda r: r[0]["cases"][0]["renders"].pop(),
            lambda r: r[0]["cases"][0]["renders"][0]["frames"].pop(),
            lambda r: r[0]["cases"][0]["renders"][0].update(checksum=99),
            lambda r: r[0]["cases"][0]["renders"][0]["quality"].update(maximum_color_error_bytes=2),
            lambda r: r[0]["cases"][0]["renders"][0]["frames"][0]["gpu"].update(valid_bits=0),
            lambda r: r[0]["cases"][0]["renders"][0]["frames"][0]["gpu"].update(submission_ms=float("nan")),
            lambda r: r[0]["cases"][0]["storage"].update(conversion_live_payload_bytes=0),
            lambda r: r[0]["cases"][0]["conversion_process_memory"].update(internal_scratch_exact=True),
            lambda r: r[0]["cases"][0]["after_release"].update(allocation_bytes=10),
        ]
        for mutate in mutations:
            with self.subTest(mutation=mutate):
                runs = fixture()
                mutate(runs)
                with self.assertRaises((ValueError, KeyError)):
                    module.aggregate(runs)


if __name__ == "__main__":
    unittest.main()
