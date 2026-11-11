[[vk::binding(0,0)]] Texture2D<float> sceneDepth;
[[vk::binding(1,0)]] RWStructuredBuffer<float> values;
struct Frame { uint4 extent; };
[[vk::push_constant]] ConstantBuffer<Frame> frame;
[shader("compute")]
[numthreads(8,8,1)]
void computeMain(uint3 tid : SV_DispatchThreadID) {
    uint width=frame.extent.x,height=frame.extent.y;
    if(tid.x<width && tid.y<height) values[tid.y*width+tid.x]=sceneDepth.Load(int3(tid.xy,0));
}
