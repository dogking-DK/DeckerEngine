[[vk::binding(2,0)]] StructuredBuffer<float4> parameters;
[[vk::binding(3,0)]] RWStructuredBuffer<float4> output;
#include "path-accumulate.slang"
[shader("compute")]
[numthreads(64,1,1)]
void emptyMain(uint3 id:SV_DispatchThreadID) {
    uint4 image=asuint(parameters[0]),tile=asuint(parameters[11]); if(id.x>=image.x*image.y) return;
    uint2 coordinate=uint2(id.x%image.x,id.x/image.x)+tile.xy;
    uint pixel=tile.w!=0 ? coordinate.y*tile.z+coordinate.x : id.x;
    Accumulation a=beginAccumulation(pixel);
    for(uint sample=image.z;sample<image.z+image.w;++sample)
        addSample(a,parameters[3].xyz,sample,float4(-1,0,0,0),0);
    finishAccumulation(a,pixel,id.x);
}
