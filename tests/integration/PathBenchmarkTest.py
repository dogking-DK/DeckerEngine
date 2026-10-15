"""Reject misleading benchmark evidence without requiring a GPU."""
import copy
import importlib.util
import json
from pathlib import Path
import shutil
import tempfile
import unittest

spec = importlib.util.spec_from_file_location("benchmark", Path(__file__).resolve().parents[2]/"scripts/benchmark-pathtracing.py")
benchmark = importlib.util.module_from_spec(spec)
spec.loader.exec_module(benchmark)


def quality_report():
    image = [[1., 1., 1.] for _ in range(16)]
    return dict(schema=1, status="passed", kind="quality", validation=True, timestamps=False, smoke=False,
                cpu_live_allocations=0, vma_live_allocations=0, diagnostics=dict(errors=0, warnings=0),
                cases=[dict(input=dict(name=name), dynamic=dict(reset_fresh=True, camera_fresh=True, pending_cancel_recovery=True),
                            seeds=[dict(seed=seed, cpu_n_spp=16384, cpu_4n_spp=65536, gpu_spp=65536,
                                        cpu_n=copy.deepcopy(image), cpu_4n=copy.deepcopy(image), gpu_4n=copy.deepcopy(image),
                                        progressive=[dict(completed_spp=spp, linear=copy.deepcopy(image)) for spp in benchmark.LEVELS])
                                   for seed in benchmark.SEEDS]) for name in benchmark.CASES])


def performance_report():
    report = quality_report()
    report.update(kind="performance", validation=False, timestamps=True, smoke=True, tracy_connected=False)
    for case in report["cases"]:
        case["workload"] = dict(width=160, height=90, max_bounces=8, batch_spp=1, tile_edge=64, frames=2)
        profile = dict(period_ns=1, valid_bits=64, submission=1, submission_ms=1, zones=[dict(name="submission", gpu_ms=1)])
        memory = dict(current=1024, peak=2048, limit=2**30)
        case["frames"] = [dict(index=i, phase="steady" if i else "cold", completed_spp=i+1, wall_ms=10,
                              memory=copy.deepcopy(memory), tiles=[dict(record_submit_ms=.1, wait_poll_ms=.2,
                                gpu=dict(profile, submission=i*6+j+1)) for j in range(6)]) for i in range(2)]
        case.update(tone=dict(gpu=profile), linear_readback=dict(gpu=profile), upload_build_gpu=profile,
                    memory_steady=memory, memory_inflight_with_old=memory, cancel_call_ms=.1, cancel_reclaim_ms=.2,
                    quality_ladder=[dict(seed=r["seed"], levels=[dict(spp=v["completed_spp"], linear=v["linear"], cumulative_ms=i+1)
                                   for i, v in enumerate(r["progressive"])]) for r in case["seeds"]])
    return report


class Acceptance(unittest.TestCase):
    def test_archived_delivery_and_corruption(self):
        root = Path(__file__).resolve().parents[2]
        spec = importlib.util.spec_from_file_location("evidence", root/"scripts/verify-pathtracing-evidence.py")
        evidence = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(evidence)
        source = root/"spec/benchmarks/2026-10-15-pathtracing"
        self.assertEqual(len(evidence.verify(source)["rounds"]), 3)
        with tempfile.TemporaryDirectory() as temporary:
            target = Path(temporary)/"evidence"
            shutil.copytree(source, target)
            archive = json.loads((target/"archive.json").read_text())
            archive["files"].pop("round-3.json.gz")
            (target/"archive.json").write_text(json.dumps(archive))
            with self.assertRaisesRegex(ValueError, "Incomplete archive"):
                evidence.verify(target)
            shutil.copyfile(source/"archive.json", target/"archive.json")
            with (target/"round-1.json.gz").open("ab") as file:
                file.write(b"corrupt")
            with self.assertRaisesRegex(ValueError, "Changed archive"):
                evidence.verify(target)

    def test_exact_reference(self):
        result = benchmark.quality(quality_report())
        self.assertEqual([r["quality_target_spp"] for r in result], [64]*3)

    def test_quality_rejections(self):
        mutations = [
            lambda r: r.update(validation=False),
            lambda r: r["cases"].pop(),
            lambda r: r["cases"][0]["seeds"].pop(),
            lambda r: r["cases"][0]["seeds"][0].update(cpu_4n_spp=16384),
            lambda r: r["cases"][0]["seeds"][0]["cpu_n"][0].__setitem__(0, 3),
            lambda r: r["cases"][0]["seeds"][0]["gpu_4n"][0].__setitem__(0, 3),
            lambda r: r["cases"][0]["seeds"][0]["progressive"][2]["linear"][0].__setitem__(0, 3),
            lambda r: r["cases"][0]["seeds"][0]["cpu_n"][0].__setitem__(0, float("nan")),
            lambda r: r["cases"][0]["dynamic"].update(pending_cancel_recovery=False),
            lambda r: r.update(vma_live_allocations=1),
        ]
        for index, mutate in enumerate(mutations):
            with self.subTest(index=index):
                report = quality_report()
                mutate(report)
                with self.assertRaises(ValueError):
                    benchmark.quality(report)

    def test_performance_rejections(self):
        truth = benchmark.quality(quality_report())
        self.assertEqual(len(benchmark.performance(performance_report(), truth, smoke=True)), 3)
        mutations = [
            lambda r: r.update(validation=True),
            lambda r: r.update(tracy_connected=True),
            lambda r: r["cases"][0]["frames"].pop(),
            lambda r: r["cases"][0]["frames"][0]["tiles"].pop(),
            lambda r: r["cases"][0]["frames"][0]["tiles"][1]["gpu"].update(submission=1),
            lambda r: r["cases"][0]["frames"][0]["tiles"][0]["gpu"].update(valid_bits=0),
            lambda r: r["cases"][0]["frames"][0]["memory"].update(peak=2**30+1),
            lambda r: r["cases"][0]["quality_ladder"].pop(),
        ]
        for index, mutate in enumerate(mutations):
            with self.subTest(index=index):
                report = performance_report()
                mutate(report)
                with self.assertRaises(ValueError):
                    benchmark.performance(report, truth, smoke=True)
        with self.assertRaises(ValueError):
            benchmark.performance(performance_report(), truth)


if __name__ == "__main__":
    unittest.main()
