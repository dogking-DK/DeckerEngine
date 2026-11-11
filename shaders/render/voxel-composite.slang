#if defined(DK_VERTEX)
[shader("vertex")]
float4 vertexMain(uint id : SV_VulkanVertexID) : SV_Position {
    float2 points[3]={float2(-1,-1),float2(3,-1),float2(-1,3)}; return float4(points[id],0,1);
}
#else
struct VoxelPixel { float4 colorDepth; float4 normalT; int4 cell; uint4 identity; };
[[vk::binding(0,0)]] StructuredBuffer<VoxelPixel> pixels;
struct Frame { uint4 extent; };
[[vk::push_constant]] ConstantBuffer<Frame> frame;
struct Output { float4 color : SV_Target0; float depth : SV_Depth; };
[shader("fragment")]
Output fragmentMain(float4 position : SV_Position) {
    VoxelPixel pixel=pixels[uint(position.y)*frame.extent.x+uint(position.x)];
    if(pixel.identity.x==0) discard;
    Output result; result.color=float4(pixel.colorDepth.xyz,1); result.depth=pixel.colorDepth.w; return result;
}
#endif
