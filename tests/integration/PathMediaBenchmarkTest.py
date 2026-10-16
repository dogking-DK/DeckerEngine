"""Reject incomplete, nonfinite, stale and mismatched M17.6 evidence."""
import copy
import importlib.util
import json
from pathlib import Path
import unittest
import tempfile

spec = importlib.util.spec_from_file_location('media', Path(__file__).parents[2]/'scripts/benchmark-path-media.py')
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)


def performance():
    memory = dict(current=100, peak=200, limit=1 << 30, vma_block_bytes=256)
    cases = []
    for name in m.CASES:
        cases.append(dict(name=name, width=640, height=360, geometry=42, environment=dict(enabled=True), field_hash=11,
                          eye=[8, 1.8, 0], target=[-4, 2, 0], cpu_scene_ms=1, gpu_prepare_ms=1,
                          comparison=dict(samples=4, linear=[[1, 2, 3]]*576, second=[[1, 4, 9]]*576),
                          preview=[1]*12, cancel=[dict(reclaim_ms=1)]*12, memory=memory,
                          frames=[dict(completed_spp=i, wall_ms=1, memory=memory,
                                       tiles=[dict(gpu=dict(period_ns=1, valid_bits=64, zones=[1], submission_ms=1))]*60)
                                  for i in range(1, 5)]))
    return dict(schema=1, status='passed', kind='performance', validation=False, smoke=False, configuration='Release',
                model=4, sampler=2, time=17, bounces=4, cpu_live=0, vma_live=0, diagnostics=dict(errors=0, warnings=0),
                device='test', driver='test', tile=64, cases=cases)


def rounds(*values):
    return [dict(v, fixture_round=i) for i, v in enumerate(values)]


def quality():
    value = performance(); value.update(kind='quality', validation=True)
    value['cpu_quality'] = [dict(name=n, seeds=[dict(seed=s, cpu_n=[1]*3, cpu_4n=[1]*3, gpu=[1]*3, second=[1]*3)
                                               for s in m.SEEDS])
                            for n in ('air-surface', 'cloud-surface', 'rough-reflection', 'near-mirror')]
    image = lambda count: dict(samples=count, linear=[[1]*3 for _ in range(16)], second=[[1]*3 for _ in range(16)])
    for c in value['cases']:
        c['seeds'] = [dict(seed=s, reference_seed=s+104729, reference_n=image(16384), reference_4n=image(65536),
                           levels=[dict(spp=n, increment_ms=1, image=image(n)) for n in m.LEVELS]) for s in m.SEEDS]
    return value


class Evidence(unittest.TestCase):
    def test_complete_and_identical(self):
        a = performance()
        self.assertEqual(len(m.performance(rounds(a, a, a))), 10)
        self.assertTrue(all(c['max_abs']['linear'] == 0 for c in m.equivalent(a, a)))

    def test_missing_round_timestamp_or_interaction(self):
        a = performance()
        with self.assertRaises(ValueError): m.performance([a, a])
        with self.assertRaises(ValueError): m.performance([a, a, a])
        for key in ('preview', 'cancel'):
            b = copy.deepcopy(a); b['cases'][0][key].pop()
            with self.assertRaises(ValueError): m.performance(rounds(a, a, b))
        b = copy.deepcopy(a); b['cases'][0]['frames'][0]['tiles'].pop()
        with self.assertRaises(ValueError): m.performance(rounds(a, a, b))

    def test_smoke_validation_or_leak(self):
        for key, value in (('smoke', True), ('validation', True), ('cpu_live', 1), ('vma_live', 1), ('model', 3)):
            b = performance(); b[key] = value
            with self.assertRaises(ValueError): m.performance(rounds(b, b, b))

    def test_changed_input_or_pixels(self):
        a = performance()
        for key, value in (('geometry', 43), ('field_hash', 12), ('environment', dict(enabled=False))):
            b = copy.deepcopy(a); b['cases'][0][key] = value
            with self.assertRaises(ValueError): m.equivalent(a, b)
        for value in (float('nan'), float('inf'), -1, 2):
            b = copy.deepcopy(a); b['cases'][0]['comparison']['linear'][0][0] = value
            with self.assertRaises(ValueError): m.equivalent(a, b)

    def test_exceeded_memory(self):
        b = performance(); b['cases'][0]['memory']['peak'] = (1 << 30)+1
        with self.assertRaises(ValueError): m.performance(rounds(b, b, b))

    def test_unconverged_reference_cannot_certify_time(self):
        value = quality()
        self.assertEqual(m.quality(value)['sponza'][0]['quality_spp'], 64)
        for row in value['cases'][0]['seeds']:
            row['reference_n']['linear'][0][0] = 3
        result = m.quality(value)['sponza'][0]
        self.assertFalse(result['reference_ready'])
        self.assertIsNone(result['quality_time_ms'])

    def test_bad_cpu_reference_or_missing_seed(self):
        value = quality(); value['cpu_quality'][0]['seeds'][0]['cpu_n'] = [9]*3
        with self.assertRaises(ValueError): m.quality(value)

    def test_archive_survives_git_lf_and_rejects_changed_bytes(self):
        base = Path(__file__).resolve().parents[2]/'out/media-aggregation-tests'; base.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=base) as directory:
            root = Path(directory).resolve(); self.assertTrue(root.is_relative_to(base.resolve()))
            source = root/'raw'; target = root/'archive'; source.mkdir()
            payloads = dict(quality=quality())
            for kind in ('baseline', 'optimized'):
                for i, row in enumerate(rounds(performance(), performance(), performance()), 1):
                    payloads[f'{kind}-{i}'] = row
                payloads[kind+'-manifest'] = dict(binary='a'*64, shaders=dict(shader='b'), assets=dict(asset='c'))
            for name, value in payloads.items():
                (source/(name+'.json')).write_bytes(json.dumps(value, indent=2).replace('\n', '\r\n').encode())
            m.archive(source, target)
            for path in target.glob('*.json'):
                self.assertNotIn(b'\r\n', path.read_bytes())
            m.verify_archive(target)
            path = target/'summary.json'; path.write_bytes(path.read_bytes()+b' ')
            with self.assertRaises(ValueError): m.verify_archive(target)
        value = quality(); value['cases'][0]['seeds'].pop()
        with self.assertRaises(ValueError): m.quality(value)


if __name__ == '__main__':
    unittest.main()
