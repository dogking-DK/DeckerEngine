#include "CloudTypes.slang"
[[vk::binding(0,0)]] ConstantBuffer<CloudParameters> cp;
struct ShadowRay { float4 geometry,weather,base,detail; };
[[vk::binding(1,0)]] StructuredBuffer<ShadowRay> rays;
[[vk::binding(2,0)]] [[vk::image_format("r32f")]] RWTexture2D<float> outputT;
[[vk::binding(3,0)]] Texture2D<float2> weatherField;
[[vk::binding(4,0)]] Texture3D<float> baseField;
[[vk::binding(5,0)]] Texture3D<float> detailField;
[[vk::binding(6,0)]] SamplerState linearRepeat;
#include "CloudTransport.slang"
[numthreads(8,8,1)]
void computeMain(uint3 id:SV_DispatchThreadID) {
    if(any(id.xy>=cp.counts.w)) return;
    ShadowRay input=rays[id.y*cp.counts.w+id.x];
    float optical=0;
    if(cp.flags.x!=0 && cp.cloud.x>0 && cp.cloud.y>0 && cp.cloud.z>0 && input.geometry.z>0) {
        CloudRayData ray; ray.weather=input.weather; ray.base=input.base; ray.detail=input.detail;
        float ds=input.geometry.z/cp.counts.x;
        [loop] for(uint i=0;i<cp.counts.x;++i) {
            float t=(float(i)+.5)*ds;
            optical+=cloudDensity(ray,cp.sun.xyz*t,cloudHeightAt(input.geometry.x,input.geometry.y,t))*cp.cloud.z*ds;
        }
    }
    outputT[id.xy]=exp(-optical);
}
