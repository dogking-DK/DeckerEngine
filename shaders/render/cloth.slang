struct VertexOutput { float4 position : SV_Position; float2 uv : TEXCOORD0; float light : TEXCOORD1; float ground : TEXCOORD2; };
#if defined(DK_VERTEX)
struct DrawData { float4 row0; float4 row1; float4 row2; float4 row3; uint columns; uint rows; float floorY; uint unused; };
[[vk::push_constant]] ConstantBuffer<DrawData> view;
[[vk::binding(0,0)]] StructuredBuffer<float4> positions;
[shader("vertex")]
VertexOutput vertexMain(uint id : SV_VulkanVertexID)
{
    VertexOutput output;
    static const uint corners[6] = {0,2,1,1,2,3};
    uint corner = corners[id % 6];
    float3 p;
    if (id < 6) {
        p = float3((corner%2)*1.7-0.85,view.floorY-0.004,(corner/2)*1.6-0.2);
        output.uv = float2(corner%2,corner/2); output.light = 1; output.ground = 1;
    } else {
        uint cell = (id-6)/6;
        uint col = cell % (view.columns-1), row = cell / (view.columns-1);
        uint base = row*view.columns+col;
        uint index = base+(corner/2)*view.columns+corner%2;
        p = positions[index].xyz;
        float3 a = positions[base].xyz, b = positions[base+1].xyz, c = positions[base+view.columns].xyz;
        float3 n = cross(b-a,c-a);
        float magnitude = length(n);
        output.light = magnitude>1e-8 ? 0.4+0.6*abs(dot(n/magnitude,normalize(float3(0.3,1,0.5)))) : 0.4;
        output.uv = float2(col+corner%2,row+corner/2); output.ground = 0;
    }
    float4 world = float4(p,1);
    output.position = float4(dot(view.row0,world),dot(view.row1,world),dot(view.row2,world),dot(view.row3,world));
    return output;
}
#else
[shader("fragment")]
float4 fragmentMain(VertexOutput input) : SV_Target0
{
    if (input.ground>0.5) {
        float2 f = frac(input.uv*16);
        float grid = (f.x<0.025 || f.y<0.025) ? 0.06 : 0;
        return float4(float3(0.12,0.16,0.21)+grid,1);
    }
    uint parity = (uint(floor(input.uv.x))+uint(floor(input.uv.y)))%2;
    float3 color = parity==0 ? float3(0.12,0.72,0.9) : float3(0.07,0.4,0.67);
    float2 edge = min(frac(input.uv),1-frac(input.uv));
    if (min(edge.x,edge.y)<0.025) color *= 0.5;
    return float4(color*input.light,1);
}
#endif
