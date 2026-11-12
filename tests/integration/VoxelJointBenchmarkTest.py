"""Corruption tests for the joint evidence boundary; no GPU needed."""
import copy
import importlib.util
from pathlib import Path
import unittest
spec = importlib.util.spec_from_file_location("joint", Path(__file__).resolve().parents[2]/"scripts/benchmark-voxel-joint.py")
joint = importlib.util.module_from_spec(spec); spec.loader.exec_module(joint)

def profile(): return dict(submission_ms=.5, valid_bits=64, period_ns=1, zones=[dict(name="submission", ms=.5)])
def memory(owned=False):
    m = dict(allocations=2, allocation_bytes=1024, block_bytes=2048, device_local_bytes=1024, host_visible_bytes=512)
    if owned: m.update(owned=dict(current=512, peak=1024, limit=4096), sbt_region_bytes=128)
    return m

def view(path, samples, width=640, height=480, projection="perspective"):
    frames=[]
    for i in range(samples+3):
        f=dict(phase="cold" if i == 0 else "warmup" if i < 3 else "steady", cpu_submit_ms=.2, wait_ms=.7, end_to_end_ms=1.)
        if path: f.update(spp=i+1, gpu_ms=.5, tiles=[dict(record_submit_ms=.2, wait_poll_ms=.7, gpu=profile())])
        else: f.update(readback_ms=.1, gpu=profile())
        frames.append(f)
    v=dict(width=width, height=height, projection=projection, frames=frames)
    if path:
        v.update(linear=dict(pixels=width*height,max_layout_error=0,max_scaled_layout_error=0,gpu=profile()),tone=dict(gpu=profile()),
            memory_steady=memory(True),memory_old_and_pending=memory(True),memory_released_view=memory(True))
    else: v.update(nano_gpu_payload_bytes=512,palette_payload_bytes=32,quality=dict(pixels=1024,maximum_color_error_bytes=0,maximum_depth_error=0))
    return v

def quality():
    return [dict(model=model,edge=edge,seeds=[dict(seed=101+7919*i,spp=4096,rays=[dict(cpu=[.1]*3,cpu_second=[.02]*3,native=[.1]*3,native_second=[.02]*3,triangle=[.1]*3) for _ in range(2)]) for i in range(8)])
        for model in ("lambert","ggx") for edge in joint.EDGES]

def run(round=0, smoke=False):
    samples=2 if smoke else 8
    d=dict(schema=1,kind="voxel-joint-baseline",matrix_version=2,measurement_version=1,status="passed",tracy_connected=False,
        diagnostics=dict(errors=0,warnings=0),heap_live_allocations=0,device_only=memory(),after_release=memory(),round=round,
        shader_fingerprint=123,warmup=2,seed=2313,max_bounces=4,configuration="Debug" if smoke else "Release",validation=smoke,smoke=smoke,
        steady_samples=samples,program_create_ms=1.,statistics=quality() if smoke else None,device=dict(name="synthetic"),compiler="synthetic",
        source_sha256="a"*64,executable_sha256="b"*64,
        precondition_raster=view(False,0 if smoke else 61,64 if smoke else 1280,48 if smoke else 720),
        precondition_path=view(True,0 if smoke else 29,64 if smoke else 640,48 if smoke else 480),
        precision_rejection=dict(span=131072,reason="voxel precision offset rejected"),cases=[])
    for name in joint.SMOKE if smoke else joint.CASES:
        empty=name.startswith("empty"); count=0 if empty else 1
        sizes=((640,480),) if smoke else ((640,480),(1280,720))
        c=dict(name=name,domain_min=[0,0,0],domain_max=[63,63,63],source_build_ms=1,conversion_ms=2,
            storage=dict(source_tree_bytes=128,normalized_tree_bytes=128,nano_bytes=512,nodes=[1,1,1],tiles=[1,0,0],active_voxels=0 if empty else 512,content_hash=123),
            conversion_process_memory=dict(samples=1,interval_ms=1,internal_scratch_exact=False,sampled_working_peak=2,working_before=1,sampled_private_peak=2,private_before=1),
            before=memory(),after_release=memory(),raster_upload=dict(gpu=profile()),raster=[view(False,samples,w,h,p) for w,h in sizes for p in ("perspective","orthographic")],layouts=[])
        for edge in joint.EDGES:
            geometry=dict(triangles=16,nano=512 if count else 0,instances=208,blocks=count*32,palette=32,aabb_input=count*24,blas_storage=count*512,tlas_storage=count*128,as_scratch_requirement=256*count)
            c["layouts"].append(dict(edge=edge,cpu_scene_ms=1,block_count=count,geometry=geometry,geometry_reused=edge!=8,
                prepare=dict(cpu_submit_ms=1,wall_ms=2,gpu=profile() if count and edge==8 else None),primary=dict(rays=1024,hits=count,oracle="dense_dda"),
                memory_build_with_old=memory(True),memory_scene=memory(True),views=[view(True,samples,w,h,p) for w,h in sizes for p in ("perspective","orthographic")]))
            if smoke and (name.startswith("sparse") or name=="solid256"): c["layouts"][-1]["regression"]=[view(True,8,w,h) for w,h in ((640,480),(1280,720))]
        d["cases"].append(c)
    return d

class EvidenceTest(unittest.TestCase):
    def test_complete_rounds_and_pooled_statistics(self):
        result=joint.aggregate([run(i) for i in range(3)],run(smoke=True))
        self.assertEqual(len(result["rows"]),12*4*4)
        self.assertEqual(result["rows"][0]["gpu_ms"]["n"],24)
        self.assertEqual(len(result["statistics"]),12)
    def test_missing_duplicate_and_mixed_sources(self):
        for change in (lambda d:d.update(round=0),lambda d:d.update(shader_fingerprint=999),lambda d:d.update(executable_sha256="c"*64),lambda d:d.update(source_sha256="d"*64)):
            data=[run(i) for i in range(3)];change(data[2])
            with self.assertRaises(ValueError):joint.aggregate(data,run(smoke=True))
    def test_corrupt_gpu_quality_memory_or_matrix(self):
        corruptions=[lambda d:d["cases"].pop(), lambda d:d["cases"][0]["layouts"].pop(),lambda d:d["cases"][0]["raster"].pop(),
            lambda d:d["cases"][0]["raster"][0]["frames"].pop(),lambda d:d["cases"][0]["raster"][0]["frames"][3]["gpu"].update(submission_ms=float("nan")),
            lambda d:d["cases"][0]["after_release"].update(allocation_bytes=1025),lambda d:d["cases"][1]["layouts"][0]["geometry"].update(aabb_input=1),
            lambda d:d.update(validation=True),lambda d:d["diagnostics"].update(warnings=1),lambda d:d.update(heap_live_allocations=1),
            lambda d:d["cases"][0]["layouts"][0]["views"][0]["linear"].update(max_scaled_layout_error=.001),
            lambda d:d["cases"][0]["layouts"][0]["views"][0]["frames"][3].update(gpu_ms=99)]
        for change in corruptions:
            data=run();change(data)
            with self.assertRaises(ValueError):joint.validate_run(data)
    def test_statistical_bias_and_seed_mismatch(self):
        data=quality();data[0]["seeds"][0]["rays"][0]["native"][0]+=.02
        with self.assertRaises(ValueError):joint.statistics_quality(data)
        data=quality();data[0]["seeds"][0]["seed"]=0
        with self.assertRaises(ValueError):joint.statistics_quality(data)
    def test_percentile_retains_slow_sample(self):
        result=joint.stats([1.]*23+[100.])
        self.assertEqual(result["maximum"],100.)
        self.assertEqual(result["n"],24)

if __name__ == "__main__": unittest.main()
