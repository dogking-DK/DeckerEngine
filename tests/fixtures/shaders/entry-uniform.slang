[[vk::binding(0, 0)]] RWStructuredBuffer<uint> values;
[shader("compute")]
[numthreads(1, 1, 1)]
void computeMain(uniform uint value, uint3 index : SV_DispatchThreadID) { values[index.x] = value; }
