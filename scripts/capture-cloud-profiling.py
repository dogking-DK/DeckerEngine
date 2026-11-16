"""Capture the real optimized cloud workload and check Tracy/native durations."""
import argparse
import csv
import hashlib
import io
import json
import os
from pathlib import Path
import subprocess
import time

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', type=Path, default=ROOT / 'out/build/windows-graphics-profiling')
    parser.add_argument('--tools-dir', type=Path, default=ROOT / 'out/profiling-tools/vcpkg_installed/x64-windows/tools/tracy')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--port', type=int, default=18089)
    args = parser.parse_args()
    binary = (args.build_dir / 'bin/RelWithDebInfo/dk-cloud-benchmark.exe').resolve()
    capture = (args.tools_dir / 'tracy-capture.exe').resolve()
    exporter = (args.tools_dir / 'tracy-csvexport.exe').resolve()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    env = os.environ.copy()
    env.update(TRACY_PORT=str(args.port), TRACY_ONLY_LOCALHOST='1', TRACY_ONLY_IPV4='1',
               TRACY_NO_EXIT='0', DISABLE_LAYER_AMD_SWITCHABLE_GRAPHICS_1='1')
    for key in ('VK_LAYER_VALIDATE_SYNC', 'VK_VALIDATION_VALIDATE_SYNC'):
        env.pop(key, None)
    report = dict(status='failed', started_at=time.strftime('%Y-%m-%dT%H:%M:%S%z'),
                  binary=str(binary), native_passes_checked=0,
                  boundary='Profiling ON, connected, validation OFF. Separate from performance baseline.')
    child = None
    try:
        version = subprocess.check_output([str(exporter), '--version'], text=True).strip()
        report['tools_version'] = version
        if '0.14.1' not in version:
            raise ValueError('Expected Tracy 0.14.1 tools')
        trace, measurement = output / 'cloud.tracy', output / 'native.json'
        command = [str(binary), '--wait-tracy', '--timestamps', '--preset', 'overcast',
                   '--mode', 'combined-static', '--output', str(measurement)]
        report['command'] = command
        with (output / 'probe.log').open('w') as log:
            child = subprocess.Popen(command, cwd=ROOT, env=env, stdout=log, stderr=subprocess.STDOUT,
                                     creationflags=subprocess.CREATE_NO_WINDOW)
            with (output / 'capture.log').open('w') as capture_log:
                subprocess.run([str(capture), '-a', '127.0.0.1', '-p', str(args.port), '-o', str(trace), '-s', '20'],
                               cwd=ROOT, env=env, stdout=capture_log, stderr=subprocess.STDOUT,
                               timeout=60, check=True, creationflags=subprocess.CREATE_NO_WINDOW)
            if child.wait(timeout=45):
                raise RuntimeError('Cloud probe failed; see probe.log')
        native = json.loads(measurement.read_text())
        if native['status'] != 'passed' or not native['tracy_compiled'] or native['validation']:
            raise ValueError('Invalid profiling measurement')
        tables = {}
        for kind, flag in (('gpu', '-g'), ('cpu', '-u')):
            result = subprocess.run([str(exporter), flag, str(trace)], capture_output=True, text=True, check=True, timeout=30)
            (output / f'{kind}-zones.csv').write_text(result.stdout, encoding='utf-8')
            tables[kind] = list(csv.DictReader(io.StringIO(result.stdout)))
            report[f'{kind}_zones'] = len(tables[kind])
        for name in ('GPU submission', 'half resolution cloud transport', 'depth preserving cloud reconstruction',
                     'completed cloud history reprojection'):
            if not any(z['name'] == name for z in tables['gpu']):
                raise ValueError(f'Missing GPU zone: {name}')
        for name in ('Shader.compile', 'CloudBenchmark.field_prepare', 'graph.execute', 'graphics.submit', 'graphics.wait'):
            if not any(z['name'] == name for z in tables['cpu']):
                raise ValueError(f'Missing CPU zone: {name}')
        # The trace also includes untimed memory/counter frames. Match every
        # timed native leaf pass to a distinct trace event, within endpoint rounding.
        events = {}
        for zone in tables['gpu']:
            events.setdefault(zone['name'], []).append(int(zone['GPU execution time']))
        for frame in native['frames']:
            profiles = [frame['gpu'], *frame['lut_builds'].values()]
            if frame['shadow_gpu']:
                profiles.append(frame['shadow_gpu'])
            for profile in profiles:
                for zone in profile['zones']:
                    if zone['name'] == 'GPU submission':
                        continue
                    candidates = events.get(zone['name'], [])
                    expected = zone['gpu_ms'] * 1e6
                    match = next((i for i, value in enumerate(candidates) if abs(value - expected) <= 2), None)
                    if match is None:
                        raise ValueError(f'Native/Tracy duration mismatch: {zone}')
                    candidates.pop(match)
                    report['native_passes_checked'] += 1
        report['trace_bytes'] = trace.stat().st_size
        report['trace_sha256'] = hashlib.sha256(trace.read_bytes()).hexdigest()
        report['status'] = 'passed'
    except Exception as error:
        report['error'] = str(error)
        raise
    finally:
        if child is not None and child.poll() is None:
            child.kill()
            child.wait(timeout=15)
        report['finished_at'] = time.strftime('%Y-%m-%dT%H:%M:%S%z')
        (output / 'summary.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
