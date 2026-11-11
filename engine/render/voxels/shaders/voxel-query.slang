#include "voxel-hdda.slang"
[[vk::binding(0,0)]] ConstantBuffer<DkVoxelParameters> parameters;
[[vk::binding(1,0)]] StructuredBuffer<uint> voxelWords;
[[vk::binding(2,0)]] StructuredBuffer<DkVoxelRay> rays;
[[vk::binding(3,0)]] RWStructuredBuffer<DkVoxelResult> results;
[shader("compute")]
[numthreads(64,1,1)]
void computeMain(uint3 tid : SV_DispatchThreadID) {
    if (tid.x >= parameters.control.x) return;
    uint count, stride; voxelWords.GetDimensions(count,stride);
    DkVoxelBuffer buffer = {voxelWords,parameters.control.y,stride != 4 || count != parameters.control.y/4};
    if (buffer.fault) { DkVoxelResult bad=(DkVoxelResult)0; bad.meta.x=6; results[tid.x]=bad; return; }
    results[tid.x]=dkQueryVoxel(buffer,rays[tid.x],parameters);
}
