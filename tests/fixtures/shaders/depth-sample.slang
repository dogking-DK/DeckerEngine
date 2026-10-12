[[vk::binding(0, 0)]] Texture2D<float> depthImage;
[[vk::binding(1, 0)]] SamplerState nearestSampler;
[[vk::binding(2, 0)]] RWStructuredBuffer<float> result;
[shader("compute")]
[numthreads(1, 1, 1)]
void computeMain(uint3 id : SV_DispatchThreadID) {
    result[id.y * 4 + id.x] = depthImage.SampleLevel(nearestSampler, (float2(id.xy) + 0.5) / 4.0, 0);
}
