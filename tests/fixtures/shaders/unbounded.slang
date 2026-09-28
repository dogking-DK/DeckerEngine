[[vk::binding(0, 0)]] Texture2D<float4> images[];
[[vk::binding(1, 0)]] RWStructuredBuffer<float4> values;
[shader("compute")]
[numthreads(1, 1, 1)]
void computeMain(uint3 index : SV_DispatchThreadID) { values[index.x] = images[index.x].Load(int3(0)); }
