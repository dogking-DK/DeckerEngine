#include "voxel-hdda.slang"
struct RasterParameters {
    DkVoxelParameters grid;
    float4 origin,forward,right,up; // In index space; w: near/far/orthographic/first.
    float4 projection; // p22, p23, width, height.
    float4 light,energy;
    uint4 identity; // Stable instance rank, palette size, shading, reserved.
};
struct VoxelPixel { float4 colorDepth; float4 normalT; int4 cell; uint4 identity; };
[[vk::binding(0,0)]] ConstantBuffer<RasterParameters> parameters;
[[vk::binding(1,0)]] StructuredBuffer<uint> voxelWords;
[[vk::binding(2,0)]] StructuredBuffer<float4> palette;
[[vk::binding(3,0)]] RWStructuredBuffer<VoxelPixel> pixels;
[[vk::binding(4,0)]] RWStructuredBuffer<uint> status;
[shader("compute")]
[numthreads(64,1,1)]
void computeMain(uint3 tid : SV_DispatchThreadID) {
    uint width=uint(parameters.projection.z), height=uint(parameters.projection.w), id=tid.x;
    if(id>=width*height) return;
    VoxelPixel previous;
    if(parameters.up.w!=0) previous=(VoxelPixel)0; else previous=pixels[id];
    pixels[id]=previous;
    // Center first, then apply precomputed pixel steps. This preserves equal
    // slopes across axes instead of introducing two independent NDC divisions.
    float2 xy=float2(id%width,id/width)+.5-float2(width,height)*.5;
    float3 offset=xy.x*parameters.right.xyz+xy.y*parameters.up.xyz;
    DkVoxelRay ray=(DkVoxelRay)0;
    ray.origin=float4(parameters.origin.xyz+(parameters.right.w!=0 ? offset : float3(0)),parameters.origin.w);
    ray.direction=float4(parameters.forward.xyz+(parameters.right.w==0 ? offset : float3(0)),parameters.forward.w);
    if(any(!isfinite(ray.origin.xyz)) || any(!isfinite(ray.direction.xyz)) || all(ray.direction.xyz==0) ||
        any(abs(ray.origin.xyz)>1048576) || any(abs(ray.direction.xyz)>1048576) ||
        any((abs(ray.direction.xyz)<1e-8) && (ray.direction.xyz!=0))) { InterlockedOr(status[0],1u<<4); return; }
    uint count,stride; voxelWords.GetDimensions(count,stride);
    DkVoxelBuffer buffer={voxelWords,parameters.grid.control.y,stride!=4 || count!=parameters.grid.control.y/4};
    DkVoxelResult hit=dkQueryVoxel(buffer,ray,parameters.grid);
    if(hit.meta.x>=3) { InterlockedOr(status[0],1u<<hit.meta.x); return; }
    if(hit.meta.x!=1 || (previous.identity.x!=0 && hit.normalT.w>=previous.normalT.w)) return;
    if(hit.meta.y==0 || hit.meta.y>parameters.identity.y) { InterlockedOr(status[0],1u<<5); return; }
    float3 color=palette[hit.meta.y-1].xyz;
    if(parameters.identity.z==1) color*=parameters.energy.xyz*(max(dot(hit.normalT.xyz,parameters.light.xyz),0)/3.141592653589793);
    float z=(-parameters.projection.x*hit.normalT.w+parameters.projection.y);
    if(parameters.right.w==0) z/=hit.normalT.w;
    if(!isfinite(z) || z < -2e-6 || z > 1+2e-6 || any(!isfinite(color))) { InterlockedOr(status[0],1u<<4); return; }
    VoxelPixel result;
    result.colorDepth=float4(color,saturate(z)); result.normalT=hit.normalT; result.cell=hit.cell;
    result.identity=uint4(1,hit.meta.y,parameters.identity.x,hit.meta.z); pixels[id]=result;
}
