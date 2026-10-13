[[vk::binding(0, 0)]] Texture2D<float2> weather;
[[vk::binding(1, 0)]] Texture3D<float> base;
[[vk::binding(2, 0)]] Texture3D<float> detail;
[[vk::binding(3, 0)]] SamplerState repeatSampler;
[[vk::binding(4, 0)]] StructuredBuffer<float4> points;
[[vk::binding(5, 0)]] RWStructuredBuffer<float4> result;
[shader("compute")]
[numthreads(1, 1, 1)]
void computeMain(uint3 id : SV_DispatchThreadID) {
    float3 uvw = points[id.x].xyz;
    result[id.x] = float4(weather.SampleLevel(repeatSampler, uvw.xy, 0),
        base.SampleLevel(repeatSampler, uvw, 0), detail.SampleLevel(repeatSampler, uvw, 0));
}
