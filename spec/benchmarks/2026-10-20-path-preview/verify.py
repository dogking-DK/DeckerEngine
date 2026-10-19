import gzip, hashlib, json, math, statistics
from pathlib import Path

root=Path(__file__).resolve().parent
for name,expected_hash in json.loads((root/'evidence-sha256.json').read_text()).items():
    assert hashlib.sha256((root/name).read_bytes()).hexdigest()==expected_hash, f'Changed evidence: {name}'
def p95(xs): return sorted(xs)[math.ceil(len(xs)*.95)-1]
summary={'scope':'Sponza surface, 640x360, four bounces, same seeds/samples; three rounds; no validation; GPU timestamps', 'modes':{}}
for mode in ('classic','initial','temporal','spatial'):
    phases={}
    for phase in ('before','after'):
        docs=[json.loads(gzip.decompress((root/'raw'/f'{phase}-{mode}-{i}.json.gz').read_bytes())) for i in range(1,4)]
        assert all(d['status']=='passed' and not d['cpu_live'] and not d['vma_live'] and d['diagnostics']=={'errors':0,'warnings':0} for d in docs)
        cases=[d['cases'][0] for d in docs]
        frames=[f for c in cases for f in c['frames']]
        tiles=[t for f in frames for t in f['tiles']]
        phases[phase]={
            'tile': docs[0]['tile'], 'frame_wall_ms':statistics.median(f['wall_ms'] for f in frames),
            'round_frame_median_ms':[statistics.median(f['wall_ms'] for f in c['frames']) for c in cases],
            'frame_gpu_ms':statistics.median(sum(t['gpu']['submission_ms'] for t in f['tiles']) for f in frames),
            'record_submit_ms':statistics.median(sum(t['record_submit_ms'] for t in f['tiles']) for f in frames),
            'wait_poll_ms':statistics.median(sum(t['wait_poll_ms'] for t in f['tiles']) for f in frames),
            'submissions_per_spp':len(frames[0]['tiles']),
            'tile_gpu_p95_ms':p95([t['gpu']['submission_ms'] for t in tiles]),
            'preview_p95_ms':p95([v for c in cases for v in c['preview']]),
            'cancel_p95_ms':p95([v['reclaim_ms'] for c in cases for v in c['cancel']]),
            'owned_peak':max(c['memory']['peak'] for c in cases)}
    phases['speedup']=phases['before']['frame_wall_ms']/phases['after']['frame_wall_ms']
    for i in range(1,4):
        before=json.loads(gzip.decompress((root/'raw'/f'before-{mode}-{i}.json.gz').read_bytes()))['cases'][0]
        after=json.loads(gzip.decompress((root/'raw'/f'after-{mode}-{i}.json.gz').read_bytes()))['cases'][0]
        assert before['comparison']==after['comparison'],f'Changed sample result {mode} {i}'
        for b,a in zip(before['frames'],after['frames']):
            assert b['source']==a['source'] and b['transport']==a['transport'],f'Changed frame provenance/counters {mode} {i}'
    phases['comparison_exact']=True
    summary['modes'][mode]=phases
print(json.dumps(summary,indent=2))
expected=json.loads((root/'summary.json').read_text()); assert summary['modes']==expected['modes']; print('All mode aggregates and exact comparisons verified')
