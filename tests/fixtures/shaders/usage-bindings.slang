struct Constants { float4 scale; };
[[vk::binding(4, 2)]] ConstantBuffer<Constants> constants;
[[vk::binding(0, 1)]] Texture2D<float4> textures[3];
[[vk::binding(3, 1)]] SamplerState linearSampler;
[[vk::binding(1, 2)]] [format("rgba8")] RWTexture2D<float4> outputImage;
[[vk::binding(0, 2)]] RWByteAddressBuffer outputBytes;

[shader("compute")]
[numthreads(1, 1, 1)]
void computeMain(uint3 index : SV_DispatchThreadID)
{
    float4 value = textures[1].SampleLevel(linearSampler, float2(0.5), 0) * constants.scale;
    outputImage[index.xy] = value;
    outputBytes.Store((index.y * 4 + index.x) * 4, asuint(value.x));
}
