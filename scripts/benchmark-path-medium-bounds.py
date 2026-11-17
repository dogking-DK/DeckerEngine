"""0180 serial GPU ablation. Timings never certify an unconverged quality reference."""
import argparse
import array
import hashlib
import json
import math
from pathlib import Path
import statistics
import subprocess
import time
import zipfile

ROOT = Path(__file__).resolve().parents[1]
MODES = {'baseline': ['--no-density-culling', '--no-local-majorants'],
         'default': ['--no-local-majorants'], 'piecewise': ['--local-majorants']}
CASES = ('low-sun', 'courtyard-clear', 'sky-overcast', 'cloud-inside', 'surface')


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def stats(values):
    assert values and all(math.isfinite(v) and v >= 0 for v in values)
    return {'p50': statistics.median(values), 'p95': sorted(values)[math.ceil(.95*len(values))-1]}


def collect(binary, out, cases):
    import os
    out.mkdir(parents=True, exist_ok=True)
    assert not (out/'manifest.json').exists(), 'Use a fresh output directory'
    sources = sorted({p for directory in ('engine/render/pathtracing', 'engine/render/clouds',
                                         'engine/render/voxels')
                      for p in (ROOT/directory).rglob('*')
                      if p.suffix in ('.cpp', '.hpp', '.inl', '.slang')} |
                     {ROOT/'engine/framework/services/src/PathCapture.cpp', ROOT/'tests/integration/PathMediaBenchmark.cpp',
                      ROOT/'tests/integration/PathBenchmarkSupport.hpp', Path(__file__), ROOT/'vcpkg.json'})
    manifest = {'base_commit': subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip(),
                'binary': str(binary), 'binary_sha256': sha(binary), 'started_at': time.strftime('%Y-%m-%dT%H:%M:%S%z'),
                'sources_sha256': {str(p.relative_to(ROOT)): sha(p) for p in sources}, 'runs': []}
    with zipfile.ZipFile(out/'source.zip', 'w', zipfile.ZIP_DEFLATED) as z:
        for p in sources:
            z.write(p, p.relative_to(ROOT))
    env = dict(os.environ, DISABLE_LAYER_AMD_SWITCHABLE_GRAPHICS_1='1')
    for case in cases:
        modes = list(MODES) if case == 'low-sun' else ['baseline', 'default']
        for repeat in range(3):
            for mode in modes[repeat % len(modes):]+modes[:repeat % len(modes)]:
                name = f'{case}-{mode}-{repeat+1}'
                command = [str(binary), '--root', str(ROOT/'projects/demo'), '--case', case,
                           '--algorithm', 'classic', '--tile', '256', '--tiles-per-submission', '4',
                           '--warmup-samples', '256', '--performance-samples', '20',
                           '--output', str(out/(name+'.json'))] + MODES[mode]
                if case == 'low-sun' and repeat == 0 and mode != 'piecewise':
                    command += ['--linear-directory', str(out/(name+'-linear'))]
                start = time.monotonic()
                with (out/(name+'.log')).open('w', encoding='utf-8') as log:
                    result = subprocess.run(command, cwd=ROOT, env=env, stdout=log, stderr=subprocess.STDOUT, timeout=300)
                manifest['runs'].append({'name': name, 'command': command, 'exit': result.returncode,
                                         'wall_seconds': time.monotonic()-start})
                (out/'manifest.json').write_text(json.dumps(manifest, indent=2), encoding='utf-8')
                print(name, result.returncode, flush=True)
    assert sha(binary) == manifest['binary_sha256'], 'Binary changed during collection'
    assert all(sha(ROOT/p) == digest for p, digest in manifest['sources_sha256'].items()), 'Source changed during collection'
    manifest['finished_at'] = time.strftime('%Y-%m-%dT%H:%M:%S%z')
    (out/'manifest.json').write_text(json.dumps(manifest, indent=2), encoding='utf-8')


def analyze(out):
    manifest = json.loads((out/'manifest.json').read_text(encoding='utf-8'))
    summary = {'runs': [], 'groups': {}, 'failures': [], 'exact_comparisons': []}
    inputs = {}
    for run in manifest['runs']:
        d = json.loads((out/(run['name']+'.json')).read_text(encoding='utf-8'))
        if run['exit'] or d['status'] != 'passed':
            summary['failures'].append({'name': run['name'], 'error': d.get('error')})
            continue
        assert d['diagnostics'] == {'errors': 0, 'warnings': 0} and not d['cpu_live'] and not d['vma_live']
        assert (d['model'], d['sampler'], d['time'], d['bounces'], d['tile'], d['warmup_samples']) == (4, 2, 17, 4, 256, 256)
        assert not d['validation'] and not d['smoke'] and d['tiles_per_submission'] == 4
        c = d['cases'][0]; frames = c['frames']; assert len(frames) == 20
        mode = run['name'].rsplit('-', 2)[-2]
        assert d['medium_acceleration'] == {'density_culling': mode != 'baseline', 'local_majorants': mode == 'piecewise'}
        identity = {k: c[k] for k in ('geometry', 'fingerprint', 'textures', 'triangles', 'environment', 'field_hash', 'eye', 'target')}
        assert inputs.setdefault(c['name'], identity) == identity, 'Different input across rounds'
        for i, f in enumerate(frames):
            assert f['source']['samples'] == f['completed_spp'] == i+1
            assert f['transport']['seed'] == 101 and f['transport']['max_bounces'] == 4

        gpu = [sum(t['gpu']['submission_ms'] for t in f['tiles']) for f in frames]
        trace = [sum(z['gpu_ms'] for t in f['tiles'] for z in t['gpu']['zones']
                     if z['name'].startswith('surface path trace tile')) for f in frames]
        row = {'name': run['name'], 'gpu_ms_per_spp': stats(gpu), 'wall_ms_per_spp': stats([f['wall_ms'] for f in frames]),
               'trace_ms_per_spp': stats(trace), 'tracking_steps_20spp': frames[-1]['source']['tracking_steps'],
               'first_warmup_ms': c.get('first_warmup_ms'), 'base_compile_ms': d['compile_ms'], 'memory': c['memory']}
        summary['runs'].append(row)
        group = run['name'].rsplit('-', 1)[0]
        summary['groups'].setdefault(group, []).append(row)
    for group, rows in list(summary['groups'].items()):
        summary['groups'][group] = {'rounds': len(rows),
            'gpu_p50': statistics.median(r['gpu_ms_per_spp']['p50'] for r in rows),
            'wall_p50': statistics.median(r['wall_ms_per_spp']['p50'] for r in rows),
            'gpu_round_p50': [r['gpu_ms_per_spp']['p50'] for r in rows]}
    for case in CASES:
        paths = [out/f'{case}-{mode}-1.json' for mode in ('baseline', 'default')]
        if not all(p.exists() for p in paths): continue
        a, b = [json.loads(p.read_text(encoding='utf-8')) for p in paths]
        if a['status'] != 'passed' or b['status'] != 'passed': continue
        a, b = a['cases'][0], b['cases'][0]
        for key in ('geometry', 'fingerprint', 'textures', 'triangles', 'environment', 'field_hash', 'eye', 'target'):
            assert a[key] == b[key], f'Input mismatch: {case}/{key}'
        error = max(abs(x-y) for key in ('linear', 'second')
                    for p, q in zip(a['comparison'][key], b['comparison'][key]) for x, y in zip(p, q))
        assert error <= 1e-6, f'Same-sample mismatch: {case}: {error}'
        assert a['frames'][-1]['source']['tracking_steps'] == b['frames'][-1]['source']['tracking_steps']
        item = {'case': case, 'linear_and_second_max_abs': error}
        if case == 'low-sun':
            images = []
            for c in (a, b):
                image_path = Path(c['frames'][-1]['linear_file'])
                if not image_path.exists(): image_path = out/image_path.parent.name/image_path.name
                values = array.array('f'); values.frombytes(image_path.read_bytes()); images.append(values)
            assert len(images[0]) == len(images[1]) == 640*360*3
            item['full_20spp_max_abs'] = max(abs(x-y) for x, y in zip(*images))
            assert item['full_20spp_max_abs'] <= 1e-6
        summary['exact_comparisons'].append(item)
    summary['status'] = 'passed' if not summary['failures'] and all(g['rounds'] == 3 for g in summary['groups'].values()) else 'failed'
    (out/'summary.json').write_text(json.dumps(summary, indent=2), encoding='utf-8')
    print(json.dumps({'groups': summary['groups'], 'failures': summary['failures'],
                      'exact_comparisons': summary['exact_comparisons']}, indent=2))
    assert summary['status'] == 'passed', 'Incomplete or failed benchmark matrix'


def quality(path, out):
    value = json.loads(path.read_text(encoding='utf-8'))
    assert value['status'] == 'passed' and value['validation'] and value['kind'] == 'quality'
    assert value['quality_extent'] == [4, 4] and value['reference_n'] == 16384
    assert value['diagnostics'] == {'errors': 0, 'warnings': 0} and not value['cpu_live'] and not value['vma_live']
    assert value['medium_acceleration'] == {'density_culling': True, 'local_majorants': False}
    rows = value['cases'][0]['seeds']
    assert [r['seed'] for r in rows] == [101+7919*i for i in range(8)]
    assert [r['reference_seed'] for r in rows] == [104729+101+7919*i for i in range(8)]
    def flat(image):
        assert len(image['linear']) == 16
        return [v for pixel in image['linear'] for v in pixel]
    def mean(images):
        return [statistics.mean(v) for v in zip(*images)]
    def error(images, reference):
        return math.sqrt(sum((x-y)**2 for image in images for x, y in zip(image, reference)) /
                         (len(images)*max(sum(x*x for x in reference), len(reference)*1e-12)))
    reference = mean([flat(r['reference_4n']) for r in rows])
    convergence = error([mean([flat(r['reference_n']) for r in rows])], reference)
    result = {'reference_n_4n_nrmse': convergence, 'reference_ready': convergence <= .005,
              'ladder': [], 'quality_spp': None, 'quality_time_ms': None}
    for index, spp in enumerate((64, 256, 1024, 4096, 16384)):
        images = []
        for r in rows:
            level = r['levels'][index]; assert level['spp'] == level['image']['samples'] == spp
            assert r['reference_n']['samples'] == 16384 and r['reference_4n']['samples'] == 65536
            images.append(flat(level['image']))
        result['ladder'].append({'spp': spp, 'nrmse': error(images, reference),
            'ensemble_nrmse': error([mean(images)], reference),
            'upper_roi': error([i[:24] for i in images], reference[:24]),
            'lower_roi': error([i[24:] for i in images], reference[24:])})
        if result['reference_ready'] and result['ladder'][-1]['nrmse'] <= .02 and result['quality_spp'] is None:
            result['quality_spp'] = spp
    # This run has validation enabled. It cannot certify a production time-to-quality.
    result['quality_time_reason'] = 'unconverged reference' if not result['reference_ready'] else 'quality run is not a performance timing'
    out.mkdir(parents=True, exist_ok=True)
    (out/'quality-summary.json').write_text(json.dumps(result, indent=2), encoding='utf-8')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--analyze', action='store_true')
    parser.add_argument('--quality-input', type=Path)
    parser.add_argument('--cases', nargs='+', choices=CASES, default=CASES)
    args = parser.parse_args()
    if args.quality_input:
        quality(args.quality_input, args.output)
    elif args.analyze:
        analyze(args.output.resolve())
    else:
        assert args.binary, '--binary required for collection'
        collect(args.binary.resolve(), args.output.resolve(), args.cases)
        analyze(args.output.resolve())
