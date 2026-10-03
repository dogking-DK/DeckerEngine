#if defined(DK_VERTEX)
[shader("vertex")]
float4 vertexMain(uint id : SV_VulkanVertexID) : SV_Position
{
    float2 points[3] = {float2(-1,-1),float2(3,-1),float2(-1,3)};
    return float4(points[id],0,1);
}
#else
[[vk::binding(0,0)]] Texture2D<float4> hdrTexture;
struct ToneData { float4 parameters; };
[[vk::push_constant]] ConstantBuffer<ToneData> tone;
float srgb(float x) { return x <= 0.0031308 ? 12.92*x : 1.055*pow(x,1.0/2.4)-0.055; }
[shader("fragment")]
float4 fragmentMain(float4 position : SV_Position) : SV_Target0
{
    float3 value = max(hdrTexture.Load(int3(int2(position.xy),0)).rgb,0) * tone.parameters.x;
    float3 mapped = 1.0 - 1.0 / (1.0 + value);
    return float4(srgb(mapped.r),srgb(mapped.g),srgb(mapped.b),1);
}

#endif
