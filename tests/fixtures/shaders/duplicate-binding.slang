[[vk::binding(0, 0)]] RWStructuredBuffer<uint> first;
[[vk::binding(0, 0)]] RWStructuredBuffer<uint> second;
[shader("compute")]
[numthreads(1, 1, 1)]
void computeMain(uint3 index : SV_DispatchThreadID) { first[index.x] = second[index.x]; }
