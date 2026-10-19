from pathlib import Path
import gzip,hashlib,json,math,statistics
root=Path(__file__).resolve().parent
manifest=json.loads((root/'manifest.json').read_text(encoding='utf-8'))
for name,digest in manifest.items():
    assert hashlib.sha256((root/name).read_bytes()).hexdigest()==digest,name
def load(name):
    return json.loads(gzip.decompress((root/'raw'/name).read_bytes()))
def p95(xs):
    return sorted(xs)[math.ceil(.95*len(xs))-1]
result={}
for mode in ['classic','initial','spatial']:
    reference=load('before-'+mode+'-1.json.gz')['cases'][0]
    values={}
    for phase in ['before','after']:
        wall=[]; gpu=[]; cpu=[]; ratios=[]; tile=[]; cancel=[]; submissions=[]
        for round in range(1,4):
            report=load(f'{phase}-{mode}-{round}.json.gz'); case=report['cases'][0]
            assert report['status']=='passed' and report['cpu_live']==0 and report['vma_live']==0
            assert report['diagnostics']=={'errors':0,'warnings':0}
            assert case['comparison']==reference['comparison'],(phase,mode,round,'linear')
            for frame,old in zip(case['frames'],reference['frames']):
                assert frame['source']==old['source'] and frame['transport']==old['transport']
                assert frame['memory']['peak']<=frame['memory']['limit']
                # First sample includes pipeline/driver warmup. Retain it in raw, exclude from warm aggregate.
                if frame['completed_spp']==1: continue
                duration=frame['wall_ms']; busy=sum(t['gpu']['submission_ms'] for t in frame['tiles'])
                wall.append(duration); gpu.append(busy); ratios.append(100*busy/duration)
                cpu.append(sum(t['record_submit_ms'] for t in frame['tiles'])); submissions.append(len(frame['tiles']))
                tile.extend(t['gpu']['submission_ms'] for t in frame['tiles'])
            cancel.extend(x['reclaim_ms'] for x in case['cancel'])
            for key in ['motion','recovery']:
                if key=='motion':
                    assert [x['source'] for x in case[key]]==[x['source'] for x in reference[key]]
                else: assert case[key]['source']==reference[key]['source']
        values[phase]={key:statistics.median(xs) for key,xs in [('wall_ms',wall),('gpu_ms',gpu),('record_ms',cpu),('gpu_interval_percent',ratios),('submissions',submissions)]}
        values[phase].update(tile_p95_ms=p95(tile),cancel_p95_ms=p95(cancel))
    values['speedup']=values['before']['wall_ms']/values['after']['wall_ms']; result[mode]=values
desktop={}
for round in range(1,4):
    report=load(f'desktop-{round}.json.gz'); assert report['status']=='passed' and not report['validation']
    for row in report['samples']:
        assert row['owned_peak']<=row['owned_limit']
        if row['spp']>1: desktop.setdefault(f"{row['width']}x{row['height']}-mode{row['mode']}",[]).append(row['interval_ms'])
print(json.dumps({'modes':result,'desktop_warm_ms':{k:statistics.median(v) for k,v in desktop.items()}},indent=2))
