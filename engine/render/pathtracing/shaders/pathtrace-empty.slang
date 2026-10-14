[[vk::binding(2,0)]] StructuredBuffer<float4> parameters;
[[vk::binding(3,0)]] RWStructuredBuffer<float4> output;
[shader("compute")]
[numthreads(64,1,1)]
void emptyMain(uint3 id:SV_DispatchThreadID) {
    uint4 image=asuint(parameters[0]); if(id.x>=image.x*image.y) return;
    float3 color=parameters[3].xyz;
    output[id.x*4]=float4(color,0); output[id.x*4+1]=float4(color*color,0);
    output[id.x*4+2]=float4(-1,0,0,0); output[id.x*4+3]=0;
}
