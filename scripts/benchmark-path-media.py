"""M17.6 fixed-input quality, sample equivalence and serial performance evidence."""
import argparse
import hashlib
import gzip
import json
import math
from pathlib import Path
import statistics
import subprocess
import time

CASES = ('surface', 'courtyard-clear', 'courtyard-sparse', 'sky-clear', 'sky-sparse',
         'sky-overcast', 'low-sun', 'fog', 'cloud-inside', 'cloud-above')
SEEDS = [101 + 7919*i for i in range(8)]
LEVELS = [64, 256, 1024, 4096, 16384]


def need(value, message):
    if not value:
        raise ValueError(message)


def stats(values):
    need(values and all(math.isfinite(v) and v >= 0 for v in values), 'Invalid measurement')
    return dict(n=len(values), median=statistics.median(values), p95=sorted(values)[math.ceil(.95*len(values))-1], max=max(values))


def flat(image, count):
    need(len(image) == count and all(len(p) == 3 for p in image), 'Wrong image dimensions')
    values = [v for p in image for v in p]
    need(all(isinstance(v, (int, float)) and math.isfinite(v) and v >= 0 for v in values), 'Invalid radiance')
    return values


def mean(images):
    return [statistics.mean(v) for v in zip(*images)]


def nrmse(images, reference):
    return math.sqrt(sum((a-b)**2 for image in images for a, b in zip(image, reference)) /
                     (len(images)*max(sum(v*v for v in reference), len(reference)*1e-12)))


def common(value, kind):
    need(value['schema'] == 1 and value['status'] == 'passed' and value['kind'] == kind, 'Failed or mislabeled run')
    need(not value['smoke'] and value['configuration'] == 'Release', 'Smoke/Debug is not acceptance evidence')
    need((value['model'], value['sampler'], value['time'], value['bounces']) == (4, 2, 17, 4), 'Changed physical workload')
    need(not value['cpu_live'] and not value['vma_live'] and value['diagnostics'] == dict(errors=0, warnings=0), 'Leak or validation diagnostic')
    need([c['name'] for c in value['cases']] == list(CASES), 'Missing/duplicate case')


def quality(value):
    common(value, 'quality')
    need(value['validation'], 'Quality requires validation')
    cpu = []
    need([c['name'] for c in value['cpu_quality']] == ['air-surface', 'cloud-surface', 'rough-reflection', 'near-mirror'], 'Missing CPU reference')
    for case in value['cpu_quality']:
        rows = case['seeds']
        need([r['seed'] for r in rows] == SEEDS, 'Missing CPU seed')
        for r in rows:
            for k in ('cpu_n', 'cpu_4n', 'gpu', 'second'):
                flat([r[k]], 1)
        reference = mean([r['cpu_4n'] for r in rows])
        convergence = nrmse([mean([r['cpu_n'] for r in rows])], reference)
        agreement = nrmse([mean([r['gpu'] for r in rows])], reference)
        need(convergence <= .005, f"{case['name']}: CPU N/4N unconverged: {convergence}")
        need(agreement <= .02, f"{case['name']}: CPU/GPU mismatch: {agreement}")
        for channel in range(3):
            a = [r['cpu_4n'][channel] for r in rows]
            b = [r['gpu'][channel] for r in rows]
            se = math.sqrt((statistics.variance(a)+statistics.variance(b))/8)
            need(abs(statistics.mean(a)-statistics.mean(b)) <= 5*se+1e-4, 'CPU/GPU outside confidence bound')
        cpu.append(dict(name=case['name'], n_4n_nrmse=convergence, cpu_gpu_nrmse=agreement, reference=reference))
    results = []
    for case in value['cases']:
        rows = case['seeds']
        need([r['seed'] for r in rows] == SEEDS and [r['reference_seed'] for r in rows] == [104729+s for s in SEEDS], 'Wrong independent reference seeds')
        n, four = [], []
        for r in rows:
            for key, count in (('reference_n', 16384), ('reference_4n', 65536)):
                need(r[key]['samples'] == count, 'Changed reference range')
                flat(r[key]['second'], 16)
            n.append(flat(r['reference_n']['linear'], 16)); four.append(flat(r['reference_4n']['linear'], 16))
            need([v['spp'] for v in r['levels']] == LEVELS, 'Changed quality ladder')
        reference = mean(four)
        convergence = nrmse([mean(n)], reference)
        # A reference which has not stabilized cannot certify a quality crossing.
        reference_ready = convergence <= .005
        ladder = []
        for index, spp in enumerate(LEVELS):
            images = []
            for r in rows:
                level = r['levels'][index]
                need(level['image']['samples'] == spp, 'Wrong candidate range')
                images.append(flat(level['image']['linear'], 16)); flat(level['image']['second'], 16)
            times = [sum(v['increment_ms'] for v in r['levels'][:index+1]) for r in rows]
            ladder.append(dict(spp=spp, nrmse=nrmse(images, reference), time_ms=stats(times),
                               upper_roi=nrmse([i[:24] for i in images], reference[:24]),
                               lower_roi=nrmse([i[24:] for i in images], reference[24:])))
        reached = next((v for v in ladder if reference_ready and v['nrmse'] <= .02), None)
        luminance = [statistics.mean(i) for i in four]
        results.append(dict(name=case['name'], reference_ready=reference_ready, n_4n_nrmse=convergence,
                            reference_mean=statistics.mean(luminance), ci95_half_width=2.365*statistics.stdev(luminance)/math.sqrt(8),
                            ladder=ladder, quality_spp=reached['spp'] if reached else None,
                            quality_time_ms=reached['time_ms'] if reached else None))
    return dict(cpu=cpu, sponza=results)


def performance(rounds):
    need(len(rounds) == 3, 'Exactly three serial rounds required')
    need(len({hashlib.sha256(json.dumps(r, sort_keys=True).encode()).hexdigest() for r in rounds}) == 3, 'Duplicated round evidence')
    for value in rounds:
        common(value, 'performance'); need(not value['validation'], 'Performance validation must be off')
        need((value['device'], value['driver'], value['tile']) == (rounds[0]['device'], rounds[0]['driver'], rounds[0]['tile']), 'Mixed adapter/driver/schedule')
    results = []
    for index, name in enumerate(CASES):
        cases = [r['cases'][index] for r in rounds]
        tiles, frames, preview, cancel, memory = [], [], [], [], []
        for case in cases:
            need((case['width'], case['height']) == (640, 360), 'Changed performance extent')
            need([f['completed_spp'] for f in case['frames']] == [1, 2, 3, 4], 'Missing frame')
            need(len(case['preview']) == len(case['cancel']) == 12, 'Missing interaction trials')
            preview += case['preview']; cancel += [c['reclaim_ms'] for c in case['cancel']]
            need(case['comparison']['samples'] == 4, 'Changed equivalence range')
            flat(case['comparison']['linear'], 576); flat(case['comparison']['second'], 576)
            for frame in case['frames']:
                frames.append(frame['wall_ms']); memory.append(frame['memory'])
                expected = math.ceil(640/rounds[0]['tile'])*math.ceil(360/rounds[0]['tile'])
                need(len(frame['tiles']) == expected, 'Incomplete tile timestamps')
                for tile in frame['tiles']:
                    gpu = tile['gpu']
                    need(gpu['period_ns'] > 0 and gpu['valid_bits'] > 0 and gpu['zones'], 'Invalid GPU timestamp')
                    tiles.append(gpu['submission_ms'])
            memory.append(case['memory'])
        need(all(0 < m['limit'] and 0 <= m['current'] <= m['peak'] <= m['limit'] for m in memory), 'GPU budget exceeded')
        results.append(dict(name=name, cpu_scene_ms=stats([c['cpu_scene_ms'] for c in cases]),
                            gpu_prepare_ms=stats([c['gpu_prepare_ms'] for c in cases]), spp_ms=stats(frames),
                            first_complete_ms=stats([c['frames'][0]['wall_ms'] for c in cases]),
                            preview_96x54_ms=stats(preview), cancel_reclaim_ms=stats(cancel), tile_gpu_ms=stats(tiles),
                            owned_peak=max(m['peak'] for m in memory), vma_block_peak=max(m['vma_block_bytes'] for m in memory)))
    return results


def equivalent(before, after):
    common(before, 'performance'); common(after, 'performance')
    need((before['device'], before['driver']) == (after['device'], after['driver']), 'Different hardware')
    results = []
    for a, b in zip(before['cases'], after['cases']):
        for key in ('name', 'geometry', 'environment', 'field_hash', 'eye', 'target'):
            need(a[key] == b[key], 'Changed input: '+key)
        errors = {}
        for key in ('linear', 'second'):
            x, y = flat(a['comparison'][key], 576), flat(b['comparison'][key], 576)
            error = max(abs(u-v) for u, v in zip(x, y)); need(error <= 1e-6, 'Fixed samples changed: '+a['name'])
            errors[key] = error
        results.append(dict(name=a['name'], max_abs=errors))
    return results


def sha(path):
    return hashlib.file_digest(path.open('rb'), 'sha256').hexdigest()


def manifest(executable, shaders, root):
    return dict(binary=sha(executable), shaders={p.name: sha(p) for p in sorted(shaders.rglob('*.slang'))},
                assets={str(p.relative_to(root)): sha(p) for p in sorted(root.rglob('*'))
                        if p.is_file() and not any(part.startswith('.') for part in p.relative_to(root).parts)})


def aggregate(read):
    before = [read(f'baseline-{i}.json') for i in range(1, 4)]
    after = [read(f'optimized-{i}.json') for i in range(1, 4)]
    old, new = read('baseline-manifest.json'), read('optimized-manifest.json')
    for source in (old, new):
        need(len(source['binary']) == 64 and source['shaders'] and source['assets'], 'Missing input hashes')
    need(old['assets'] == new['assets'], 'Changed source asset bytes')
    reference = read('quality.json')
    for a, b in zip(after[0]['cases'], reference['cases']):
        for key in ('name', 'geometry', 'environment', 'field_hash', 'eye', 'target'):
            need(a[key] == b[key], 'Quality/performance workload mismatch: '+key)
    proof = quality(reference)
    result = dict(schema=1, status='passed', quality=proof, baseline=performance(before), optimized=performance(after),
                  equivalence=[equivalent(a, b) for a, b in zip(before, after)], manifests=dict(baseline=old, optimized=new))
    return result


def summarize(directory):
    result = aggregate(lambda name: json.loads((directory/name).read_text(encoding='utf-8')))
    (directory/'summary.json').write_text(json.dumps(result, indent=2), encoding='utf-8')
    return result


def archive(source, destination):
    summarize(source); destination.mkdir(parents=True, exist_ok=True)
    names = ['summary.json', 'baseline-manifest.json', 'optimized-manifest.json', 'quality.json',
             *[f'{kind}-{i}.json' for kind in ('baseline', 'optimized') for i in range(1, 4)]]
    if (source/'editor.json').is_file(): names.append('editor.json')
    index = dict(schema=1, files={})
    for name in names:
        raw = (source/name).read_bytes()
        compressed = name not in ('summary.json', 'baseline-manifest.json', 'optimized-manifest.json')
        # Git normalizes these text files to LF. Hash their committed bytes;
        # compressed raw measurements retain the original byte stream.
        if not compressed: raw = raw.replace(b'\r\n', b'\n')
        data = gzip.compress(raw, mtime=0) if compressed else raw
        target = name+'.gz' if compressed else name
        (destination/target).write_bytes(data)
        index['files'][name] = dict(file=target, sha256=hashlib.sha256(data).hexdigest(), raw_sha256=hashlib.sha256(raw).hexdigest())
    (destination/'archive.json').write_bytes(json.dumps(index, indent=2).encode('utf-8'))


def verify_archive(directory):
    index = json.loads((directory/'archive.json').read_text(encoding='utf-8'))
    expected = {'summary.json', 'baseline-manifest.json', 'optimized-manifest.json', 'quality.json',
                *[f'{kind}-{i}.json' for kind in ('baseline', 'optimized') for i in range(1, 4)]}
    need(index['schema'] == 1 and set(index['files']) in (expected, expected | {'editor.json'}), 'Incomplete archive')
    def read(name):
        entry = index['files'][name]
        need(Path(entry['file']).name == entry['file'], 'Invalid archive path')
        data = (directory/entry['file']).read_bytes()
        need(hashlib.sha256(data).hexdigest() == entry['sha256'], 'Changed archive bytes')
        raw = gzip.decompress(data) if entry['file'].endswith('.gz') else data
        need(hashlib.sha256(raw).hexdigest() == entry['raw_sha256'], 'Changed raw bytes')
        return json.loads(raw)
    result = aggregate(read)
    need(result == read('summary.json'), 'Summary does not reproduce')
    if 'editor.json' in index['files']:
        editor = read('editor.json')
        need(len(editor['workers']) == 3, 'Missing worker rounds')
        for worker in editor['workers']:
            need(worker['status'] == 'passed' and not worker['validation'] and worker['profile'], 'Invalid worker evidence')
            need(not worker['diagnostics'] and not worker['live_allocations'], 'Worker leak/diagnostic')
            need((worker['width'], worker['height'], worker['model'], worker['sampler'], worker['seed'], worker['time'], worker['bounces']) ==
                 (192, 108, 4, 2, 1, 17, 4), 'Changed worker workload')
            need([c['preset'] for c in worker['media']] == ['clear', 'sparse', 'overcast'], 'Missing worker case')
            for case in worker['media']:
                need(len(case['camera_ms']) == 12 and len(case['cancel_ms']) == 13, 'Missing worker trials')
                need(stats(case['camera_ms'])['p95'] <= 100 and stats(case['cancel_ms'])['p95'] <= 100, 'Worker latency candidate not met')
                need(case['owned_peak'] <= case.get('owned_limit', 1 << 30), 'Worker memory exceeded')
        for gui in (editor['gui_validation'], editor['gui_performance']):
            need(gui['status'] == 'passed', 'GUI failed')
            need(stats(gui['media_latency_ms'])['p95'] <= 100, 'GUI latency candidate not met')
            for case in gui['checkpoints'][-4:]:
                need(case['width'] > 192 and case['spp'] == 1, 'GUI did not restore full extent')
                if case['checkpoint'].endswith('_motion'):
                    need(case['motion_updates'] >= 16 and all(max(s['width'], s['height']) <= 192 for s in case['motion_samples']), 'GUI motion starved')
    return result


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--directory', type=Path, required=True)
    p.add_argument('--executable', type=Path); p.add_argument('--root', type=Path); p.add_argument('--shaders', type=Path)
    p.add_argument('--collect', choices=('baseline', 'optimized', 'quality')); p.add_argument('--summarize', action='store_true')
    p.add_argument('--archive', type=Path); p.add_argument('--verify-archive', action='store_true')
    args = p.parse_args(); args.directory.mkdir(parents=True, exist_ok=True)
    if args.collect:
        need(all((args.executable, args.root, args.shaders)), 'Collection requires binary/root/shaders')
        frozen = manifest(args.executable, args.shaders, args.root)
        if args.collect != 'quality':
            (args.directory/(args.collect+'-manifest.json')).write_text(json.dumps(frozen, indent=2), encoding='utf-8')
        for i in (range(1, 4) if args.collect != 'quality' else [None]):
            name = f'{args.collect}-{i}' if i else 'quality'
            command = [str(args.executable), '--root', str(args.root), '--shaders', str(args.shaders), '--output', str(args.directory/(name+'.json'))]
            if args.collect == 'quality': command.append('--quality')
            start = time.time()
            with (args.directory/(name+'.stdout.log')).open('wb') as out, (args.directory/(name+'.stderr.log')).open('wb') as err:
                subprocess.run(command, stdout=out, stderr=err, check=True, timeout=14400,
                               creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
            need(manifest(args.executable, args.shaders, args.root) == frozen, 'Input changed during run')
            print(name, f'{time.time()-start:.2f}s', flush=True)
    if args.summarize:
        summarize(args.directory); print(args.directory/'summary.json')
    if args.archive:
        archive(args.directory, args.archive); verify_archive(args.archive); print(args.archive)
    if args.verify_archive:
        verify_archive(args.directory); print('Archive hashes and recomputed gates passed')


if __name__ == '__main__':
    main()
