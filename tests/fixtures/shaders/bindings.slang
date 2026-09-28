struct Constants { float4 color; float4x4 matrix; };
[[vk::binding(4, 2)]] ConstantBuffer<Constants> constants;
[[vk::binding(0, 1)]] Texture2D<float4> textures[3];
[[vk::binding(3, 1)]] SamplerState linearSampler;
[[vk::binding(1, 2)]] RWTexture2D<float4> outputImage;
[[vk::binding(0, 2)]] RWByteAddressBuffer outputBytes;

[shader("compute")]
[numthreads(8, 4, 1)]
void computeMain(uint3 index : SV_DispatchThreadID)
{
    float4 value = textures[1].SampleLevel(linearSampler, float2(0.5), 0) * constants.color;
    outputImage[index.xy] = mul(value, constants.matrix);
    outputBytes.Store(index.x * 4, asuint(value.x));
}
