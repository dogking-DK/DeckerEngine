struct DrawData { float4 row0; float4 row1; float4 row2; float4 row3; float4 baseColor; float4 emissive; };
[[vk::push_constant]] ConstantBuffer<DrawData> drawData;
float4 clipPosition(float3 position)
{
    float4 p = float4(position, 1);
    return float4(dot(drawData.row0,p),dot(drawData.row1,p),dot(drawData.row2,p),dot(drawData.row3,p));
}
struct VertexOutput { float4 position : SV_Position; float2 uv : TEXCOORD0; };
#if defined(DK_VERTEX)
[shader("vertex")]
VertexOutput vertexMain(float3 position : POSITION, float2 uv : TEXCOORD0)
{
    VertexOutput result; result.position = clipPosition(position); result.uv = uv; return result;
}
#else
[[vk::binding(0,0)]] Texture2D<float4> baseTexture;
[[vk::binding(1,0)]] SamplerState baseSampler;
float4 surface(float2 uv)
{
    float4 value = baseTexture.Sample(baseSampler,uv) * drawData.baseColor;
    // emissive.w is the alpha cutoff, or -1 for opaque materials.
    if (value.a < drawData.emissive.w) discard;
    return value;
}
#if defined(DK_DEPTH)
[shader("fragment")]
void depthMain(VertexOutput input) { surface(input.uv); }
#else
[shader("fragment")]
float4 fragmentMain(VertexOutput input) : SV_Target0
{
    return float4(surface(input.uv).rgb + drawData.emissive.rgb,1);
}
#endif
#endif
