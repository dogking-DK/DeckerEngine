"""Reject incomplete or misleading M19.8 quality/performance evidence."""
import array
import copy
import hashlib
import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location('restir', Path(__file__).parents[2]/'scripts/benchmark-restir-sponza.py')
m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m)


def source(mode, spp):
    return dict(restir=m.expected_restir(mode), samples=spp, sampling_first_epoch=0, history_epoch=None,
                empty_history=True, tracking_steps=None, temporal_max_age=8, spatial_max_age=8,
                temporal_max_candidates=72, spatial_max_candidates=320, temporal_expired=1)


def transport(seed=101, name=None):
    return dict(model=3 if name == 'surface' else 4, sampler=1 if name == 'surface' else 2,
                medium=name != 'surface', max_bounces=4, rr_start=3, seed=seed, fingerprint=2, geometry_fingerprint=1)


def common(mode, kind, identity):
    return dict(schema=1, status='passed', algorithm=mode, restir=m.expected_restir(mode), kind=kind,
                smoke=False, validation=False, configuration='Release', model=4, sampler=2, time=17, bounces=4, tile=64,
                rtxdi='3.1.0/f12037fa8e97ebc08e9e3edfd2de528ed1772a4b', reconstruction='none',
                quality_wait='active_poll_1ms_then_block',
                cpu_live=0, vma_live=0, diagnostics=dict(errors=0, warnings=0), compile_ms=1, asset_load_ms=1, cpu_peak=1,
                collection=dict(run_id=identity, manifest_sha256='a'*64))


def case(name):
    return dict(name=name, geometry=1, fingerprint=2, textures=25, triangles=262266,
                environment=dict(preset=name), field_hash=3, eye=[8, 1.8, 0], target=[-4, 2, 0], surface_only=name == 'surface')


def quality():
    result = {}
    def image(spp):
        return dict(samples=spp, linear=[[1., 1., 1.] for _ in range(16)], second=[[1., 1., 1.] for _ in range(16)])
    for mode in m.MODES:
        for name in m.CASES:
            run = common(mode, 'quality', mode+name)
            run.update(timing_quality=True, quality_extent=[4, 4], quality_limit=16384, reference_n=16384,
                       skip_reference=mode != 'classic')
            c = case(name); c.update(width=4, height=4, seeds=[])
            for seed in m.SEEDS:
                r = dict(seed=seed, reference_seed=seed+104729, levels=[dict(spp=n, increment_ms=1., increment_gpu_ms=.5,
                         submissions=(n-(m.LEVELS[i-1] if i else 0))//16 if mode == 'classic' else
                                     (n-(m.LEVELS[i-1] if i else 0))*dict(initial=2, temporal=3, spatial=4)[mode],
                         image=image(n), source=source(mode, n), transport=transport(seed,name)) for i, n in enumerate(m.LEVELS)])
                if mode == 'classic':
                    r.update(reference_n=dict(image(16384), source=source('classic', 16384), transport=transport(seed+104729,name)),
                             reference_4n=dict(image(65536), source=source('classic', 65536), transport=transport(seed+104729,name)))
                c['seeds'].append(r)
            run['cases'] = [c]; result[mode, name] = run
    return result


def performance(mode):
    result = []
    for run_id in range(3):
        run = common(mode, 'performance', mode+str(run_id)); run.update(preview_extent=[640, 360], performance_samples=4, cases=[])
        memory = dict(current=100, peak=200, limit=1 << 30, vma_block_bytes=256)
        for name in m.CASES:
            c = case(name); c.update(width=640, height=360, cpu_scene_ms=1, gpu_prepare_ms=1, frames=[],
                                    preview=[10]*12, cancel=[dict(reclaim_ms=1) for _ in range(12)], motion=[], memory=memory)
            for i in range(12):
                s = source(mode, 1)
                if mode in ('temporal', 'spatial') and i:
                    s.update(sampling_first_epoch=i, history_epoch=i-1, empty_history=False)
                c['motion'].append(dict(shift=.01*(i+1), source=s, transport=transport(name=name)))
            c['recovery'] = dict(completed_spp=16, source=source(mode, 16), transport=transport(name=name))
            for i in range(1, 5):
                tile = dict(gpu=dict(valid_bits=64, period_ns=1, submission_ms=1, zones=[dict(name='trace', gpu_ms=1)]))
                c['frames'].append(dict(completed_spp=i, source=source(mode, i), transport=transport(name=name), wall_ms=200, memory=memory,
                                        tiles=[copy.deepcopy(tile) for _ in range(60*dict(classic=1, initial=2, temporal=3, spatial=4)[mode])]))
            run['cases'].append(c)
        result.append(run)
    return result


class Evidence(unittest.TestCase):
    def test_wait_equivalence_rejects_changed_pixels_or_missing_chain(self):
        run = quality()['spatial', 'surface']
        for row in run['cases'][0]['seeds']:
            row['levels'] = row['levels'][:1]
        proof = dict(before=run, after=copy.deepcopy(run), binary_before='a'*64, binary_after='b'*64)
        proof['before'] = copy.deepcopy(run); proof['before'].pop('quality_wait')
        m.wait_equivalent(proof)
        proof['after']['cases'][0]['seeds'][0]['levels'][0]['image']['linear'][0][0] += .001
        with self.assertRaises(ValueError): m.wait_equivalent(proof)
        proof['after'] = copy.deepcopy(run); proof['after']['cases'][0]['seeds'].pop()
        with self.assertRaises(ValueError): m.wait_equivalent(proof)

    def test_complete_quality_and_four_performance_modes(self):
        q = m.quality(quality())
        self.assertTrue(all(c['reference_ready'] for c in q))
        self.assertEqual(q[0]['modes']['spatial']['quality_spp'], 64)
        for mode in m.MODES:
            p = m.performance(performance(mode), mode)
            self.assertEqual(len(p), 10)
            self.assertTrue(all(c['interaction_targets_met'] for c in p))

    def test_reference_uncertified_preserves_null_crossing(self):
        q = quality()
        for row in q['classic', 'surface']['cases'][0]['seeds']:
            row['reference_n']['linear'][0][0] = 2
        result = m.quality(q)[0]
        self.assertFalse(result['reference_ready'])
        for mode in m.MODES:
            self.assertIsNone(result['modes'][mode]['quality_wall_ms'])
            self.assertIsNone(result['modes'][mode]['equal_error_speedup'])

    def test_finite_ladder_miss_and_latency_miss_are_reported(self):
        q = quality()
        for row in q['spatial', 'surface']['cases'][0]['seeds']:
            for level in row['levels']:
                level['image']['linear'] = [[2., 2., 2.] for _ in range(16)]
        self.assertEqual(m.quality(q)[0]['modes']['spatial']['quality_status'], 'not_reached')
        p = performance('classic'); p[0]['cases'][0]['preview'] = [101]*12
        self.assertFalse(m.performance(p, 'classic')[0]['interaction_targets_met'])

    def test_missing_chain_ladder_or_inherited_history_rejected(self):
        for change in ('seed', 'level', 'history', 'nan', 'settings', 'input', 'transport', 'actual_seed', 'passes', 'wall', 'gpu'):
            q = quality(); run = q['temporal', 'surface']; c = run['cases'][0]; row = c['seeds'][0]
            if change == 'seed': c['seeds'].pop()
            if change == 'level': row['levels'].pop()
            if change == 'history': row['levels'][0]['source']['empty_history'] = False
            if change == 'nan': row['levels'][0]['image']['linear'][0][0] = float('nan')
            if change == 'settings': run['restir']['temporal']['max_history_age'] = 9
            if change == 'input': c['geometry'] = 4
            if change == 'transport': row['levels'][0]['transport']['fingerprint'] = 3
            if change == 'actual_seed': row['levels'][0]['transport']['seed'] += 1
            if change == 'passes': row['levels'][1]['submissions'] -= 1
            if change == 'wall': row['levels'][1]['increment_ms'] = -.5
            if change == 'gpu': row['levels'][1]['increment_gpu_ms'] = -.25
            with self.subTest(change=change), self.assertRaises(ValueError): m.quality(q)

    def test_missing_pass_round_or_motion_history_rejected(self):
        for change in ('round', 'tile', 'motion', 'duplicate', 'memory', 'validation'):
            p = performance('spatial'); c = p[0]['cases'][0]
            if change == 'round': p.pop()
            if change == 'tile': c['frames'][0]['tiles'].pop()
            if change == 'motion': c['motion'][1]['source']['history_epoch'] = None
            if change == 'duplicate': p[1]['collection'] = copy.deepcopy(p[0]['collection'])
            if change == 'memory': c['memory']['peak'] = (1 << 30)+1
            if change == 'validation': p[0]['validation'] = True
            with self.subTest(change=change), self.assertRaises(ValueError): m.performance(p, 'spatial')

    def test_full_image_hash_extent_and_finite_values(self):
        data = array.array('f', [1.]*(640*360*3)).tobytes()
        row = dict(linear_sha256=hashlib.sha256(data).hexdigest(), linear_extent=[640, 360])
        m.validate_image(row, data)
        with self.assertRaises(ValueError): m.validate_image(row, data[:-4])
        row['linear_extent'] = [32, 18]
        with self.assertRaises(ValueError): m.validate_image(row, data)
        bad = array.array('f', [float('nan')]*(640*360*3)).tobytes()
        row.update(linear_extent=[640, 360], linear_sha256=hashlib.sha256(bad).hexdigest())
        with self.assertRaises(ValueError): m.validate_image(row, bad)


if __name__ == '__main__':
    unittest.main()
