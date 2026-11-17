"""Paired cloud-only resolution and reconstruction measurements; serialized GPU work."""
import argparse, json, subprocess, os, time
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
os.chdir(ROOT)
import importlib.util
spec=importlib.util.spec_from_file_location('diagnostics','scripts/benchmark-environment-diagnostics.py')
mod=importlib.util.module_from_spec(spec);spec.loader.exec_module(mod)
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('output',type=Path,help='New directory for raw results, images, logs and summary')
parser.add_argument('--build-dir',type=Path,default=Path('out/build/windows-editor'))
parser.add_argument('--configuration',choices=('RelWithDebInfo','Release'),default='RelWithDebInfo')
args=parser.parse_args()
exe=(args.build_dir/'bin'/args.configuration/'dk-cloud-benchmark.exe').resolve()
root=args.output;root.mkdir(parents=True,exist_ok=False)
def device_state():
 try:
  return subprocess.run(['nvidia-smi','--query-gpu=name,pstate,temperature.gpu,clocks.gr,clocks.mem,power.draw,power.limit,utilization.gpu','--format=csv'],capture_output=True,text=True,check=True,timeout=10).stdout
 except (OSError,subprocess.SubprocessError):
  return None
def save_report(report):
 temporary=root/'summary.tmp'
 temporary.write_text(json.dumps(report,ensure_ascii=False,indent=2),encoding='utf-8')
 temporary.replace(root/'summary.json')
report={'status':'running','binary_sha256':mod.digest(exe),'started_at':time.strftime('%Y-%m-%dT%H:%M:%S%z'),'sources_sha256':{p:mod.digest(p) for p in ('engine/render/pipeline/src/ScenePipeline.cpp','shaders/render/cloud-reconstruction.slang','engine/render/clouds/include/dk/render/CloudReference.hpp','tests/integration/CloudBenchmark.cpp')},'runs':[]}
report['base_commit']=subprocess.run(['git','rev-parse','HEAD'],capture_output=True,text=True,check=True).stdout.strip()
report['driver_sha256']=mod.digest(__file__)
report['gpu_before']=device_state()
env=dict(os.environ,DISABLE_LAYER_AMD_SWITCHABLE_GRAPHICS_1='1')
for r in range(1,4):
 for preset in ('clear','sparse','overcast','sunset'):
  # Alternate route order to avoid assigning all warmed runs to the new route.
  for divisor in (2,4):
   for compact in ((False,True) if r%2 else (True,False)):
    name=f'r{r}-{preset}-d{divisor}-'+('compact' if compact else 'legacy')
    path=root/(name+'.json')
    command=[str(exe),'--preset',preset,'--resolution-divisor',str(divisor),'--timestamps','--samples','20','--mode','combined-static','--output',str(path)]
    command+=['--compact-reconstruction' if compact else '--legacy-reconstruction']
    with (root/(name+'.log')).open('w',encoding='utf-8') as log:
     subprocess.run(command,env=env,stdout=log,stderr=subprocess.STDOUT,check=True,timeout=180)
    data=json.loads(path.read_text(encoding='utf-8'));summary=mod.summarize(data)
    report['runs'].append(dict(round=r,preset=preset,divisor=divisor,compact=compact,command=command,sha256=mod.digest(path),summary=summary))
    save_report(report)
    print(name,summary['modes']['combined-static']['steady']['total_gpu_ms']['p50'],flush=True)
report['status']='passed';report['finished_at']=time.strftime('%Y-%m-%dT%H:%M:%S%z')
report['gpu_after']=device_state()
assert report['binary_sha256']==mod.digest(exe)
assert all(mod.digest(p)==sha for p,sha in report['sources_sha256'].items())
save_report(report)
