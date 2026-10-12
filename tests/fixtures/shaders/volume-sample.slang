[[vk::binding(0, 0)]] Texture3D<float4> volume;
[[vk::binding(1, 0)]] SamplerState clampSampler;
[[vk::binding(2, 0)]] SamplerState repeatSampler;
[[vk::binding(3, 0)]] RWStructuredBuffer<float4> result;
[shader("compute")]
[numthreads(1, 1, 1)]
void computeMain() {
    result[0] = volume.SampleLevel(clampSampler, float3(0.5 / 4, 0.5 / 3, 0.5 / 5), 0);
    result[1] = volume.SampleLevel(clampSampler, float3(3.5 / 4, 2.5 / 3, 4.5 / 5), 0);
    result[2] = volume.SampleLevel(clampSampler, float3(2.0 / 4, 1.25 / 3, 2.75 / 5), 0);
    result[3] = volume.SampleLevel(clampSampler, float3(0.5 / 4, 0.5 / 3, 1.3), 0);
    result[4] = volume.SampleLevel(repeatSampler, float3(0.5 / 4, 0.5 / 3, 1.3), 0);
}
