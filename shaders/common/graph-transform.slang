[[vk::binding(0, 0)]] RWStructuredBuffer<float4> vertices;
[shader("compute")]
[numthreads(1,1,1)]
void computeMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x < 3) vertices[id.x].xy *= 0.8;
}
