[[vk::binding(0,0)]] StructuredBuffer<float4> parameters;
[[vk::binding(1,0)]] StructuredBuffer<float4> history;
[[vk::binding(2,0)]] [[vk::image_format("rgba8")]] RWTexture2D<float4> display;
[shader("compute")]
[numthreads(16,16,1)]
void toneMain(uint3 id:SV_DispatchThreadID) {
    float4 p=parameters[0]; if(id.x>=uint(p.x) || id.y>=uint(p.y)) return;
    float3 linear=history[(id.y*uint(p.x)+id.x)*8].xyz*p.z;
    // This equivalent form also saturates when exposure overflows a finite HDR value.
    float3 color=1-1/(1+linear);
    if(p.w!=0) color=select(color<=.0031308,color*12.92,1.055*pow(color,1.0/2.4)-.055);
    display[id.xy]=float4(color,1);
}
