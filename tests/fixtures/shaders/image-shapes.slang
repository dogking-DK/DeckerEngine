[[vk::binding(0, 0)]] Texture2DArray<float4> layers[2];
[[vk::binding(2, 0)]] RWStructuredBuffer<float4> result;
[shader("compute")]
[numthreads(1, 1, 1)]
void computeMain() { result[0] = layers[0].Load(int4(0, 0, 1, 0)) + layers[1].Load(int4(0, 0, 0, 0)); }
