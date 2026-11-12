// M23.9 block layout v1. Subrange addresses stay local to one immutable Nano grid.
#define DK_PNANO_SUBRANGE 1
#include "voxel-hdda.slang"
[[vk::binding(48,0)]] StructuredBuffer<uint> voxelWords;
[[vk::binding(49,0)]] StructuredBuffer<float4> voxelInstances;
[[vk::binding(50,0)]] StructuredBuffer<int4> voxelBlocks;
[[vk::binding(51,0)]] StructuredBuffer<float4> voxelPalette;
float3 voxelTransform(uint instance,uint row,float3 value,float w) {
    uint base=instance*13+row; float4 p=float4(value,w);
    return float3(dot(voxelInstances[base],p),dot(voxelInstances[base+1],p),dot(voxelInstances[base+2],p));
}
float3 voxelNormal(uint instance,uint face) {
    float3 local=0; local[face/2]=(face&1) ? 1 : -1;
    return normalize(voxelTransform(instance,6,local,0));
}
PathBsdf voxelMaterial(PathPayload hit) {
    uint base=asuint(voxelInstances[hit.instance*13+11]).z+2*(hit.value-1);
    float4 c=voxelPalette[base],m=voxelPalette[base+1];
    PathBsdf result; result.color=c.xyz; result.metallic=c.w; result.roughness=m.x;
    result.ggx=asuint(voxelInstances[hit.instance*13+12]).y!=0; return result;
}
float3 voxelOffset(PathPayload hit,float3 direction) {
    float3 local=voxelTransform(hit.instance,3,hit.position,1);
    float3 d=voxelTransform(hit.instance,3,direction,0); uint axis=hit.face/2;
    local[axis]=float(hit.cell[axis])+((hit.face&1) ? 1 : 0);
    // Bound the object/world/object float round trip before moving off the face.
    // A single object-space ULP can disappear under a rotated instance transform.
    float sign=d[axis]>=0 ? 1 : -1;
    float4 row=abs(voxelInstances[hit.instance*13+3+axis]);
    float error=32*1.1920928955078125e-7*(dot(row.xyz,abs(hit.position))+row.w+abs(local[axis])+1);
    if(error>=.125) voxelError(7);
    local[axis]=nextFloat(local[axis]+sign*error,sign);
    return voxelTransform(hit.instance,0,local,1);
}
float4 voxelDiagnostic(PathPayload hit) {
    uint3 cell=uint3(hit.cell)&0x1fffff;
    return float4(hit.distance,asfloat(cell.x|(cell.y<<21)),asfloat((cell.y>>11)|(cell.z<<10)),-float(hit.instance*8+hit.face+1));
}
struct VoxelAttributes { int3 cell; uint face; uint value; };
void voxelError(uint code) { uint ignored; InterlockedOr(status[0],32|(1u<<(8+code)),ignored); }
[shader("intersection")]
void voxelIntersection() {
    uint instance=InstanceID(),base=instance*13;
    uint4 meta=asuint(voxelInstances[base+11]),count=asuint(voxelInstances[base+12]);
    if(PrimitiveIndex()>=count.x) { voxelError(0); return; }
    uint block=meta.w+PrimitiveIndex()*2;
    int3 lower=voxelBlocks[block].xyz,upper=voxelBlocks[block+1].xyz;
    float3 origin=ObjectRayOrigin(),direction=ObjectRayDirection();
    if(!all(isfinite(origin)) || !all(isfinite(direction)) || any(abs(origin)>1048576) || any(abs(direction)>1048576)) { voxelError(1); return; }
    uint words,stride; voxelWords.GetDimensions(words,stride);
    if(stride!=4 || (meta.x&3) || (meta.y&3) || meta.x/4>words || meta.y/4>words-meta.x/4) { voxelError(2); return; }
    DkVoxelBuffer buf={voxelWords,meta.y,false,meta.x/4};
    DkVoxelParameters params=(DkVoxelParameters)0;
    params.lower=asint(voxelInstances[base+9]); params.upper=asint(voxelInstances[base+10]); params.control=uint4(1,meta.y,65536,0);
    params.normalRows[0]=float4(1,0,0,0); params.normalRows[1]=float4(0,1,0,0); params.normalRows[2]=float4(0,0,1,0);
    // All block candidates traverse the same canonical full-grid prefix.
    // Per-block rebasing/clipping changes floating-point boundary decisions
    // and can hide a surface. The BVH still culls candidates; strict cell
    // ownership ensures only the containing block may report the shared hit.
    DkVoxelRay ray=(DkVoxelRay)0;
    ray.origin=float4(origin,RayTMin()); ray.direction=float4(direction,RayTCurrent());
    DkVoxelResult result=dkQueryVoxel(buf,ray,params);
    if(result.meta.x>1) { voxelError(result.meta.x); return; }
    if(result.meta.x==1 && all(result.cell.xyz>=lower) && all(result.cell.xyz<=upper)) {
        VoxelAttributes attr={result.cell.xyz,result.meta.z,result.meta.y}; ReportHit(result.normalT.w,0,attr);
    }
}
[shader("closesthit")]
void voxelHit(inout PathPayload hit,VoxelAttributes attr) {
    hit.found=1; hit.kind=1; hit.instance=InstanceID(); hit.distance=RayTCurrent(); hit.cell=attr.cell; hit.face=attr.face; hit.value=attr.value;
    float3 local=ObjectRayOrigin()+ObjectRayDirection()*hit.distance;
    local[hit.face/2]=float(hit.cell[hit.face/2])+((hit.face&1) ? 1 : 0);
    hit.position=voxelTransform(hit.instance,0,local,1);
}
