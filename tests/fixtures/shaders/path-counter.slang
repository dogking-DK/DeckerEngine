[[vk::binding(2,0)]] StructuredBuffer<float4> parameters;
[[vk::binding(3,0)]] RWStructuredBuffer<float4> output;
#include "path-accumulate.slang"
[numthreads(1,1,1)]
void counterMain() {
    Accumulation a=beginAccumulation(0);
    a.rays+=4;
    finishAccumulation(a,0,0);
}
