struct Parameters { uint count; uint first; uint end; uint unused; float dt; float gravity; float floorY; float damping; };
[[vk::push_constant]] ConstantBuffer<Parameters> params;
#if defined(DK_VELOCITY)
[[vk::binding(0,0)]] StructuredBuffer<float4> positions;
#else
[[vk::binding(0,0)]] RWStructuredBuffer<float4> positions;
#endif
#if defined(DK_PREDICT) || defined(DK_VELOCITY)
[[vk::binding(1,0)]] RWStructuredBuffer<float4> velocities;
#if defined(DK_PREDICT)
[[vk::binding(2,0)]] RWStructuredBuffer<float4> previous;
#else
[[vk::binding(2,0)]] StructuredBuffer<float4> previous;
#endif
#endif
#if defined(DK_PROJECT)
struct Constraint { uint a; uint b; float rest; float compliance; };
[[vk::binding(3,0)]] StructuredBuffer<Constraint> constraints;
[[vk::binding(4,0)]] StructuredBuffer<float4> directions;
[[vk::binding(5,0)]] RWStructuredBuffer<float> lambdas;
#endif
[shader("compute")]
[numthreads(64,1,1)]
void computeMain(uint3 id : SV_DispatchThreadID)
{
#if defined(DK_PROJECT)
    uint j = params.first + id.x;
    if (j >= params.end) return;
    Constraint c = constraints[j];
    float4 a = positions[c.a], b = positions[c.b];
    if (a.w + b.w == 0) return;
    precise float3 delta = a.xyz - b.xyz;
    precise float distance = sqrt(delta.x*delta.x + delta.y*delta.y + delta.z*delta.z);
    float3 n = distance < 1e-7 ? directions[j].xyz : delta / distance;
    precise float alpha = c.compliance / (params.dt * params.dt);
    precise float dl = (-(distance-c.rest)-alpha*lambdas[j])/(a.w+b.w+alpha);
    lambdas[j] += dl;
    a.xyz += a.w * dl * n; b.xyz -= b.w * dl * n;
    positions[c.a] = a; positions[c.b] = b;
#else
    uint i = id.x;
    if (i >= params.count) return;
    float4 p = positions[i];
#if defined(DK_PREDICT)
    previous[i] = p;
    if (p.w == 0) return;
    float3 v = velocities[i].xyz;
    float damping = 1.0 / (1.0 + params.damping * params.dt);
    v.xz *= damping; v.y = (v.y + params.dt * params.gravity) * damping;
    p.xyz += params.dt * v;
    positions[i] = p; velocities[i] = float4(v,0);
#elif defined(DK_FLOOR)
    if (p.w > 0) { p.y = max(p.y,params.floorY); positions[i] = p; }
#elif defined(DK_VELOCITY)
    if (p.w == 0) return;
    float3 v = (p.xyz - previous[i].xyz) / params.dt;
    if (p.y <= params.floorY && v.y < 0) v.y = 0;
    velocities[i] = float4(v,0);
#endif
#endif
}
