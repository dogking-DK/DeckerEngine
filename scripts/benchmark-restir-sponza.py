"""M19.8: frozen Sponza, independent chains, serial timings and reproducible evidence.

No quality gate is relaxed when a reference or a candidate fails to converge.
The 4x4 numerical ladder and 640x360 interaction timings are never extrapolated.
"""
import argparse
import array
import gzip
import hashlib
import importlib.util
import json
import math
from pathlib import Path
import statistics
import subprocess
import sys
import time
import uuid

_spec = importlib.util.spec_from_file_location('media', Path(__file__).with_name('benchmark-path-media.py'))
media = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(media)
need, stats, flat, mean, nrmse = media.need, media.stats, media.flat, media.mean, media.nrmse
CASES, SEEDS, LEVELS = media.CASES, media.SEEDS, media.LEVELS
MODES = ('classic', 'initial', 'temporal', 'spatial')
INPUTS = ('name', 'geometry', 'fingerprint', 'textures', 'triangles', 'environment', 'field_hash',
          'eye', 'target', 'surface_only')


def expected_restir(mode):
    if mode == 'classic':
        return None
    return dict(version=1, initial_candidates=8, max_candidate_evaluations=1 << 24,
                temporal=dict(version=1, max_history_candidates=64, max_history_age=8,
                              position_tolerance=.1, normal_cosine=.95) if mode in ('temporal', 'spatial') else None,
                spatial=dict(version=1, neighbor_count=4, radius=2, max_source_candidates=64,
                             position_tolerance=1, normal_cosine=.9) if mode == 'spatial' else None)


def common(value, mode, kind):
    need(value['schema'] == 1 and value['status'] == 'passed' and value['kind'] == kind, 'Failed/mislabeled run')
    need(value['algorithm'] == mode and value['restir'] == expected_restir(mode), 'Changed algorithm/settings')
    need(not value['smoke'] and not value['validation'] and value['configuration'] == 'Release', 'Invalid timing configuration')
    need((value['model'], value['sampler'], value['time'], value['bounces'], value['tile']) == (4, 2, 17, 4, 64), 'Changed workload')
    need(value['rtxdi'] == '3.1.0/f12037fa8e97ebc08e9e3edfd2de528ed1772a4b' and value['reconstruction'] == 'none', 'Changed backend')
    need(value['quality_wait'] == 'active_poll_1ms_then_block', 'Changed quality waiting protocol')
    need(not value['cpu_live'] and not value['vma_live'] and value['diagnostics'] == dict(errors=0, warnings=0), 'Leak/diagnostic')
    need(value['collection']['run_id'] and len(value['collection']['manifest_sha256']) == 64, 'Missing run provenance')
    stats([value['compile_ms'], value['asset_load_ms'], value['cpu_peak']])


def source(row, mode, samples, empty=True, seed=None):
    s = row['source']
    t = row['transport']
    need((t['model'], t['sampler'], t['medium']) in ((3, 1, False), (4, 2, True)) and
         t['max_bounces'] == 4 and t['rr_start'] == 3, 'Changed actual transport settings')
    if seed is not None:
        need(t['seed'] == seed, 'Actual seed differs from independent chain')
    need(s['restir'] == expected_restir(mode) and s['samples'] == samples, 'Image configuration/range differs')
    if empty:
        need(s['sampling_first_epoch'] == 0 and s['empty_history'] and s['history_epoch'] is None, 'Chain inherited history')
    for key, value in s.items():
        if key not in ('restir', 'history_epoch', 'tracking_steps', 'empty_history'):
            need(isinstance(value, (int, float)) and math.isfinite(value) and value >= 0, 'Invalid source counter')
    need(s['tracking_steps'] is None, 'Tracking count is not observable')
    need(s['temporal_max_age'] <= 8 and s['spatial_max_age'] <= 8, 'Unbounded history')
    need(s['temporal_max_candidates'] <= 72 and s['spatial_max_candidates'] <= 320, 'Unbounded candidates')
    return s


def same_input(a, b):
    need(all(a[k] == b[k] for k in INPUTS), 'Changed physical input')


def transport_input(row, case):
    t = row['transport']
    need(t['fingerprint'] == case['fingerprint'] and t['geometry_fingerprint'] == case['geometry']
         and t['medium'] == (not case['surface_only']), 'Image transport differs from case input')


def quality(values):
    result = []
    for case in CASES:
        runs = {m: values[m, case] for m in MODES}
        for mode, run in runs.items():
            common(run, mode, 'quality')
            need(run['timing_quality'] and run['quality_extent'] == [4, 4] and run['quality_limit'] == 16384
                 and run['reference_n'] == 16384 and run['skip_reference'] == (mode != 'classic'), 'Changed quality protocol')
            need([c['name'] for c in run['cases']] == [case], 'Missing quality case')
            c = run['cases'][0]
            need((c['width'], c['height']) == (4, 4), 'Changed quality extent')
            same_input(c, runs['classic']['cases'][0])
            need([r['seed'] for r in c['seeds']] == SEEDS and
                 [r['reference_seed'] for r in c['seeds']] == [s+104729 for s in SEEDS], 'Missing independent chain')
        reference_rows = runs['classic']['cases'][0]['seeds']
        refs, refs_n = [], []
        for row in reference_rows:
            for key, spp in (('reference_n', 16384), ('reference_4n', 65536)):
                need(row[key]['samples'] == spp, 'Changed reference range')
                transport_input(row[key], runs['classic']['cases'][0])
                source(row[key], 'classic', spp, seed=row['reference_seed'])
                flat(row[key]['second'], 16)
            refs.append(flat(row['reference_4n']['linear'], 16))
            refs_n.append(flat(row['reference_n']['linear'], 16))
        reference = mean(refs)
        convergence = nrmse([mean(refs_n)], reference)
        ready = convergence <= .005
        modes = {}
        for mode, run in runs.items():
            rows = run['cases'][0]['seeds']
            need(all([v['spp'] for v in r['levels']] == LEVELS for r in rows), 'Incomplete quality ladder')
            ladder = []
            for i, spp in enumerate(LEVELS):
                images = []
                for row in rows:
                    level = row['levels'][i]
                    delta = spp - (LEVELS[i-1] if i else 0)
                    submissions = delta//16 if mode == 'classic' else delta*dict(initial=2, temporal=3, spatial=4)[mode]
                    need(level['image']['samples'] == spp and level['submissions'] == submissions, 'Wrong candidate range/pass count')
                    stats([level['increment_ms'], level['increment_gpu_ms']])
                    transport_input(level, run['cases'][0])
                    source(level, mode, spp, seed=row['seed'])
                    images.append(flat(level['image']['linear'], 16)); flat(level['image']['second'], 16)
                wall = [sum(v['increment_ms'] for v in row['levels'][:i+1]) for row in rows]
                gpu = [sum(v['increment_gpu_ms'] for v in row['levels'][:i+1]) for row in rows]
                # Chain means are the statistical units; correlated pixels/epochs are not independent samples.
                channels = []
                for channel in range(3):
                    a = [statistics.mean(img[channel::3]) for img in images]
                    b = [statistics.mean(img[channel::3]) for img in refs]
                    bias = abs(statistics.mean(a)-statistics.mean(b))
                    se = math.sqrt((statistics.variance(a)+statistics.variance(b))/8)
                    channels.append(dict(mean=statistics.mean(a), reference=statistics.mean(b), bias=bias,
                                         combined_se=se, relative_bound=.02*abs(statistics.mean(b))+1e-4,
                                         statistical_bound=5*se+1e-4,
                                         within_bounds=bias <= .02*abs(statistics.mean(b))+1e-4 and bias <= 5*se+1e-4))
                ladder.append(dict(spp=spp, nrmse=nrmse(images, reference), mean_nrmse=nrmse([mean(images)], reference),
                                   upper_roi=nrmse([v[:24] for v in images], reference[:24]),
                                   lower_roi=nrmse([v[24:] for v in images], reference[24:]),
                                   wall_ms=stats(wall), gpu_ms=stats(gpu), channels=channels))
            crossing = next((v for v in ladder if ready and v['nrmse'] <= .02), None)
            modes[mode] = dict(ladder=ladder, quality_status='reference_uncertified' if not ready else
                              ('reached' if crossing else 'not_reached'), quality_spp=crossing['spp'] if crossing else None,
                              quality_wall_ms=crossing['wall_ms'] if crossing else None,
                              quality_gpu_ms=crossing['gpu_ms'] if crossing else None,
                              final_source=rows[-1]['levels'][-1]['source'],
                              mean_status='reference_uncertified' if not ready else
                              ('within_bounds' if all(c['within_bounds'] for c in ladder[-1]['channels']) else 'outside_bounds'),
                              long_chain_improved=ladder[-1]['nrmse'] < ladder[-2]['nrmse'])
        # A common measured time budget, using whole observed ladder levels only.
        budget = min(m['ladder'][-1]['wall_ms']['median'] for m in modes.values())
        for mode, item in modes.items():
            eligible = [r for r in item['ladder'] if r['wall_ms']['median'] <= budget]
            at_time = eligible[-1] if eligible else None
            item['equal_time'] = dict(budget_ms=budget, spp=at_time['spp'] if at_time else None,
                                     measured_ms=at_time['wall_ms']['median'] if at_time else None,
                                     nrmse=at_time['nrmse'] if at_time else None, reference_certified=ready)
            baseline = modes['classic']['quality_wall_ms']
            item['equal_error_speedup'] = baseline['median']/item['quality_wall_ms']['median'] if baseline and item['quality_wall_ms'] else None
        result.append(dict(name=case, reference_n_4n_nrmse=convergence, reference_ready=ready, modes=modes))
    return result


def performance(rounds, mode):
    need(len(rounds) == 3 and len({r['collection']['run_id'] for r in rounds}) == 3, 'Three independent runs required')
    for run in rounds:
        common(run, mode, 'performance')
        need(run['preview_extent'] == [640, 360] and run['performance_samples'] == 4, 'Changed preview/spp')
        need([c['name'] for c in run['cases']] == list(CASES), 'Missing/duplicate performance case')
    results = []
    for ci, name in enumerate(CASES):
        frames, gpu_frames, tiles, preview, cancel, memory, passes = [], [], [], [], [], [], {}
        cases = [r['cases'][ci] for r in rounds]
        for case in cases:
            same_input(case, cases[0])
            need((case['width'], case['height']) == (640, 360), 'Changed performance size')
            need([f['completed_spp'] for f in case['frames']] == [1, 2, 3, 4], 'Incomplete frame sequence')
            need(len(case['preview']) == len(case['cancel']) == len(case['motion']) == 12, 'Incomplete motion/cancel')
            preview += case['preview']; cancel += [r['reclaim_ms'] for r in case['cancel']]
            for i, row in enumerate(case['motion']):
                transport_input(row, case)
                s = source(row, mode, 1, empty=mode in ('classic', 'initial') or i == 0, seed=101)
                need(abs(row['shift']-.01*(i+1)) < 1e-9, 'Changed camera sequence')
                if mode in ('temporal', 'spatial') and i:
                    need(s['sampling_first_epoch'] == i and s['history_epoch'] == i-1 and not s['empty_history'], 'Motion lost history')
            transport_input(case['recovery'], case)
            source(case['recovery'], mode, 16, empty=mode in ('classic', 'initial'), seed=101)
            need(case['recovery']['completed_spp'] == 16, 'Stop did not recover')
            memory.append(case['memory'])
            for frame in case['frames']:
                transport_input(frame, case)
                source(frame, mode, frame['completed_spp'], seed=101)
                expected = 60 * dict(classic=1, initial=2, temporal=3, spatial=4)[mode]
                need(len(frame['tiles']) == expected, 'Missing pass/tile timestamps')
                frames.append(frame['wall_ms']); memory.append(frame['memory'])
                frame_gpu = 0
                for tile in frame['tiles']:
                    p = tile['gpu']
                    need(p['valid_bits'] > 0 and p['period_ns'] > 0 and p['zones'], 'Invalid GPU timestamps')
                    stats([p['submission_ms']]); frame_gpu += p['submission_ms']; tiles.append(p['submission_ms'])
                    for zone in p['zones']:
                        stats([zone['gpu_ms']])
                        passes.setdefault(zone['name'], []).append(zone['gpu_ms'])
                gpu_frames.append(frame_gpu)
        need(all(0 < m['limit'] and 0 <= m['current'] <= m['peak'] <= m['limit'] for m in memory), 'GPU budget exceeded')
        result = dict(name=name, spp_wall_ms=stats(frames), spp_gpu_ms=stats(gpu_frames), tile_gpu_ms=stats(tiles),
                      preview_ms=stats(preview), cancel_ms=stats(cancel),
                      cold_cpu_scene_ms=stats([c['cpu_scene_ms'] for c in cases]),
                      cold_gpu_prepare_ms=stats([c['gpu_prepare_ms'] for c in cases]),
                      owned_peak=max(m['peak'] for m in memory), vma_block_peak=max(m['vma_block_bytes'] for m in memory),
                      pass_ms={k: stats(v) for k, v in sorted(passes.items())},
                      motion_source=cases[0]['motion'][-1]['source'], recovery_source=cases[0]['recovery']['source'])
        result['interaction_targets_met'] = result['preview_ms']['p95'] <= 100 and result['cancel_ms']['p95'] <= 100 and result['tile_gpu_ms']['p95'] <= 8
        results.append(result)
    return results


def image_rows(value):
    if isinstance(value, dict):
        if 'linear_file' in value:
            yield value
        for v in value.values():
            yield from image_rows(v)
    elif isinstance(value, list):
        for v in value:
            yield from image_rows(v)


def validate_image(row, raw):
    need(hashlib.sha256(raw).hexdigest() == row['linear_sha256'], 'Changed linear image bytes')
    need(row['linear_extent'] == [640, 360] and len(raw) == 640*360*12, 'Truncated/wrong extent full image')
    pixels = array.array('f'); pixels.frombytes(raw)
    if sys.byteorder != 'little':
        pixels.byteswap()
    need(all(math.isfinite(v) and v >= 0 for v in pixels), 'Invalid full image radiance')


def report_names():
    return [f'performance-{m}-{i}.json' for i in range(1, 4) for m in MODES] + [f'quality-{m}-{c}.json' for m in MODES for c in CASES]


def aggregate(read, read_image):
    reports = {name: read(name) for name in report_names()}
    manifest = read('manifest.json')
    digest = hashlib.sha256(json.dumps(manifest, sort_keys=True).encode()).hexdigest()
    need(len(manifest['binary']) == 64 and manifest['shaders'] and manifest['assets'] and manifest['rtxdi_headers'], 'Missing source hashes')
    signatures = set(); ids = set()
    for run in reports.values():
        need(run['collection']['manifest_sha256'] == digest, 'Stale binary/assets/shaders')
        need(run['collection']['run_id'] not in ids, 'Duplicate collection identity'); ids.add(run['collection']['run_id'])
        signatures.add((run['device'], run['driver']))
        for row in image_rows(run):
            validate_image(row, read_image(row))
    need(len(signatures) == 1, 'Mixed hardware/driver')
    quality_runs = {(m, c): reports[f'quality-{m}-{c}.json'] for m in MODES for c in CASES}
    perf = {m: performance([reports[f'performance-{m}-{i}.json'] for i in range(1, 4)], m) for m in MODES}
    for mode in MODES:
        first = reports[f'performance-{mode}-1.json']
        need(len(list(image_rows(first))) == len(CASES)*3, 'Missing full-resolution image evidence')
        for c in first['cases']:
            same_input(c, quality_runs[mode, c['name']]['cases'][0])
            for row in (c['frames'][-1], c['motion'][-1], c['recovery']):
                need('linear_file' in row and 'linear_sha256' in row, 'Missing final/moved/stopped image')
    return dict(schema=1, status='complete', device=next(iter(signatures))[0], driver=next(iter(signatures))[1],
                quality_extent=[4, 4], performance_extent=[640, 360], quality=quality(quality_runs), performance=perf,
                cold={m: dict(compile_ms=stats([reports[f'performance-{m}-{i}.json']['compile_ms'] for i in range(1, 4)]),
                              asset_load_ms=stats([reports[f'performance-{m}-{i}.json']['asset_load_ms'] for i in range(1, 4)]),
                              cpu_peak=max(reports[f'performance-{m}-{i}.json']['cpu_peak'] for i in range(1, 4))) for m in MODES},
                default_policy='classic_mis', tracking_observation='unavailable',
                limitations=['4x4 quality does not certify 640x360 quality', 'renderer latency is not GUI input-to-photon latency',
                             'equal-time uses measured ladder levels without interpolation', 'finite ladder failures remain explicit'])


def read_json(path):
    return json.loads(path.read_text(encoding='utf-8'))


def write_json(path, value):
    path.write_text(json.dumps(value, indent=2, allow_nan=False)+'\n', encoding='utf-8')


def wait_equivalent(proof):
    before, after = proof['before'], proof['after']
    need(all(len(proof[k]) == 64 for k in ('binary_before', 'binary_after')), 'Missing wait comparison binary')
    need(before.get('quality_wait', 'blocking') == 'blocking', 'Missing blocking-wait baseline')
    for run in (before, after):
        need(run['status'] == 'passed' and run['algorithm'] == 'spatial' and run['kind'] == 'quality', 'Invalid wait comparison')
        need(len(run['cases']) == 1 and run['cases'][0]['name'] == 'surface', 'Changed wait comparison case')
        need([r['seed'] for r in run['cases'][0]['seeds']] == SEEDS, 'Missing wait comparison chains')
    need(after['quality_wait'] == 'active_poll_1ms_then_block', 'Missing bounded polling')
    for a, b in zip(before['cases'][0]['seeds'], after['cases'][0]['seeds']):
        need([r['spp'] for r in a['levels']] == [64] == [r['spp'] for r in b['levels']], 'Changed wait comparison samples')
        for key in ('image', 'source', 'transport'):
            need(a['levels'][0][key] == b['levels'][0][key], 'Waiting changed image/history/transport')
        flat(a['levels'][0]['image']['linear'], 16); flat(a['levels'][0]['image']['second'], 16)


def collect(args):
    need(all((args.executable, args.root, args.shaders, args.rtxdi)), 'Collection needs executable/root/shaders/rtxdi')
    def fingerprint():
        result = media.manifest(args.executable, args.shaders, args.root)
        result['rtxdi_headers'] = {str(p.relative_to(args.rtxdi)): media.sha(p) for p in sorted(args.rtxdi.rglob('*.hlsli'))}
        need(result['rtxdi_headers'], 'Missing RTXDI headers')
        return result
    frozen = fingerprint(); manifest_path = args.directory/'manifest.json'
    if manifest_path.exists():
        need(read_json(manifest_path) == frozen, 'Collection input changed; use a fresh evidence directory')
    else:
        write_json(manifest_path, frozen)
    digest = hashlib.sha256(json.dumps(frozen, sort_keys=True).encode()).hexdigest()
    modes = MODES if args.mode == 'all' else [args.mode]
    jobs = [(m, str(i), []) for i in range(1, 4) for m in modes] if args.collect == 'performance' else [
        (m, c, ['--timing-quality', '--case', c] + ([] if m == 'classic' else ['--skip-reference'])) for m in modes for c in CASES]
    for mode, suffix, extra in jobs:
        name = f'{args.collect}-{mode}-{suffix}'; output = args.directory/(name+'.json')
        if output.exists():
            need(args.resume, 'Existing run; use --resume or a fresh directory')
            try:
                existing = read_json(output)
            except (json.JSONDecodeError, UnicodeDecodeError):
                existing = {'status': 'failed'}
            if existing.get('status') == 'passed' and 'collection' in existing:
                common(existing, mode, args.collect)
                need(existing['collection']['manifest_sha256'] == digest, 'Resume stale report')
                for row in image_rows(existing):
                    validate_image(row, Path(row['linear_file']).read_bytes())
                print('resume', name, flush=True); continue
            # Preserve unsuccessful/unattested raw output before retrying this exact job.
            suffix_id = '.failed-'+uuid.uuid4().hex
            output.rename(output.with_name(name+suffix_id+'.json'))
            for stream in ('stdout', 'stderr'):
                log = args.directory/(name+'.'+stream+'.log')
                if log.exists():
                    log.rename(log.with_name(name+suffix_id+'.'+stream+'.log'))
        command = [str(args.executable.resolve()), '--root', str(args.root.resolve()), '--shaders', str(args.shaders.resolve()),
                   '--algorithm', mode, '--output', str(output.resolve()), *extra]
        if args.collect == 'performance':
            command.append('--full-preview')
            if suffix == '1':
                command += ['--linear-directory', str((args.directory/name).resolve())]
        started = time.time()
        print('start', name, flush=True)
        with (args.directory/(name+'.stdout.log')).open('wb') as out, (args.directory/(name+'.stderr.log')).open('wb') as err:
            subprocess.run(command, stdout=out, stderr=err, check=True, timeout=14400,
                           creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
        need(fingerprint() == frozen, 'Input changed during collection')
        run = read_json(output)
        run['collection'] = dict(run_id=str(uuid.uuid4()), manifest_sha256=digest, wall_seconds=time.time()-started,
                                 command=command, started_unix=started)
        for row in image_rows(run):
            raw = Path(row['linear_file']).read_bytes(); row['linear_sha256'] = hashlib.sha256(raw).hexdigest()
            validate_image(row, raw)
        common(run, mode, args.collect); write_json(output, run)
        print('complete', name, f'{time.time()-started:.2f}s', flush=True)


def archive(source_dir, destination):
    read = lambda name: read_json(source_dir/name)
    summary = aggregate(read, lambda row: Path(row['linear_file']).read_bytes())
    destination.mkdir(parents=True, exist_ok=True)
    index = dict(schema=1, files={}, images={})
    names = [*report_names(), 'manifest.json', 'summary.json']
    if (source_dir/'wait-equivalence.json').exists():
        wait_equivalent(read('wait-equivalence.json')); names.append('wait-equivalence.json')
    for name in names:
        value = summary if name == 'summary.json' else read(name)
        raw = (json.dumps(value, indent=2, allow_nan=False)+'\n').encode()
        compressed = name not in ('manifest.json', 'summary.json')
        data = gzip.compress(raw, mtime=0) if compressed else raw
        target = name+'.gz' if compressed else name
        (destination/target).write_bytes(data)
        index['files'][name] = dict(file=target, sha256=hashlib.sha256(data).hexdigest())
        for row in image_rows(value):
            sha = row['linear_sha256']
            if sha in index['images']:
                continue
            raw = Path(row['linear_file']).read_bytes(); validate_image(row, raw)
            data = gzip.compress(raw, mtime=0); target = sha+'.rgb32f.gz'; (destination/target).write_bytes(data)
            index['images'][sha] = dict(file=target, sha256=hashlib.sha256(data).hexdigest())
    write_json(destination/'archive.json', index)
    return summary


def verify_archive(directory):
    index = read_json(directory/'archive.json')
    expected = set(report_names()) | {'manifest.json', 'summary.json'}
    need(index['schema'] == 1 and set(index['files']) in (expected, expected | {'wait-equivalence.json'}), 'Incomplete archive')
    def blob(entry):
        need(Path(entry['file']).name == entry['file'], 'Invalid archive path')
        data = (directory/entry['file']).read_bytes()
        need(hashlib.sha256(data).hexdigest() == entry['sha256'], 'Changed archive bytes')
        return gzip.decompress(data) if entry['file'].endswith('.gz') else data
    read = lambda name: json.loads(blob(index['files'][name]))
    result = aggregate(read, lambda row: blob(index['images'][row['linear_sha256']]))
    need(result == read('summary.json'), 'Summary does not reproduce')
    if 'wait-equivalence.json' in index['files']:
        wait_equivalent(read('wait-equivalence.json'))
    return result


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--directory', type=Path, required=True)
    for key in ('executable', 'root', 'shaders', 'rtxdi', 'archive'):
        p.add_argument('--'+key, type=Path)
    p.add_argument('--collect', choices=('performance', 'quality'))
    p.add_argument('--mode', choices=(*MODES, 'all'), default='all')
    p.add_argument('--resume', action='store_true'); p.add_argument('--summarize', action='store_true')
    p.add_argument('--verify-archive', action='store_true')
    args = p.parse_args(); args.directory.mkdir(parents=True, exist_ok=True)
    if args.collect:
        collect(args)
    if args.summarize:
        result = aggregate(lambda n: read_json(args.directory/n), lambda r: Path(r['linear_file']).read_bytes())
        write_json(args.directory/'summary.json', result); print(args.directory/'summary.json')
    if args.archive:
        archive(args.directory, args.archive); verify_archive(args.archive); print(args.archive)
    if args.verify_archive:
        verify_archive(args.directory); print('Archive hashes, images and recomputed summary passed')


if __name__ == '__main__':
    main()
