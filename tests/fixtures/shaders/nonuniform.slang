[[vk::binding(0,0)]] Texture2D<float4> textures[64];
[[vk::binding(1,0)]] SamplerState samplers[64];
[[vk::binding(2,0)]] RWStructuredBuffer<float4> output;
[shader("compute")]
[numthreads(64,1,1)]
void computeMain(uint3 id:SV_DispatchThreadID) {
    uint index=NonUniformResourceIndex(id.x%64);
    output[id.x]=textures[index].SampleLevel(samplers[index],float2(.3,.7),0);
}
