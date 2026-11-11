struct DrawData { float4 row0,row1,row2,row3,baseColor,emissive; };
[[vk::push_constant]] ConstantBuffer<DrawData> drawData;
struct Lighting { float4 normalRows[3]; float4 light,energy; };
[[vk::binding(2,0)]] ConstantBuffer<Lighting> lighting;
struct VertexOutput { float4 position : SV_Position; float2 uv : TEXCOORD0; float3 normal : TEXCOORD1; };
#if defined(DK_VERTEX)
[shader("vertex")]
VertexOutput vertexMain(float3 position : POSITION,float2 uv : TEXCOORD0,float3 normal : NORMAL) {
    float4 p=float4(position,1); VertexOutput result;
    result.position=float4(dot(drawData.row0,p),dot(drawData.row1,p),dot(drawData.row2,p),dot(drawData.row3,p));
    result.uv=uv;
    result.normal=float3(dot(lighting.normalRows[0].xyz,normal),dot(lighting.normalRows[1].xyz,normal),dot(lighting.normalRows[2].xyz,normal));
    return result;
}
#else
[[vk::binding(0,0)]] Texture2D<float4> baseTexture;
[[vk::binding(1,0)]] SamplerState baseSampler;
float3 surface(float2 uv) {
    float4 value=baseTexture.Sample(baseSampler,uv)*drawData.baseColor;
    if(value.a<drawData.emissive.w) discard;
    return value.rgb;
}
#if defined(DK_DEPTH)
[shader("fragment")]
void depthMain(VertexOutput input) { surface(input.uv); }
#else
[shader("fragment")]
float4 fragmentMain(VertexOutput input) : SV_Target0 {
    float scale=max(max(abs(input.normal.x),abs(input.normal.y)),abs(input.normal.z));
    float3 n=scale>0 ? normalize(input.normal/scale) : float3(0);
    return float4(surface(input.uv)*lighting.energy.xyz*(max(dot(n,lighting.light.xyz),0)/3.141592653589793)+drawData.emissive.xyz,1);
}
#endif
#endif
