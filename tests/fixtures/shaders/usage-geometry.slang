#if defined(DK_GEOMETRY_COMPUTE)
[[vk::binding(0, 0)]] RWStructuredBuffer<float4> vertices;
[shader("compute")]
[numthreads(1,1,1)]
void computeMain(uint3 id : SV_DispatchThreadID)
{
    float2 positions[3] = {float2(-0.8,-0.8),float2(0.8,-0.8),float2(0,0.8)};
    if (id.x < 3) vertices[id.x] = float4(positions[id.x],0.25,1);
}
#elif defined(DK_GEOMETRY_FRAGMENT)
struct Color { float4 value; };
[[vk::push_constant]] ConstantBuffer<Color> color;
[shader("fragment")]
float4 fragmentMain() : SV_Target { return color.value; }
#else
[shader("vertex")]
float4 vertexMain(float4 position : POSITION) : SV_Position { return position; }
#endif
