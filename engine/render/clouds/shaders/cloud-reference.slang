// No jitter, history, early termination or empty-space skipping. Units: metres.
#include "CloudTypes.slang"
[[vk::binding(0,0)]] ConstantBuffer<CloudParameters> cp;
[[vk::binding(1,0)]] StructuredBuffer<CloudRayData> rays;
[[vk::binding(2,0)]] RWStructuredBuffer<CloudIntegralData> integrals;
[[vk::binding(3,0)]] Texture2D<float2> weatherField;
[[vk::binding(4,0)]] Texture3D<float> baseField;
[[vk::binding(5,0)]] Texture3D<float> detailField;
[[vk::binding(6,0)]] SamplerState linearRepeat;
#include "CloudTransport.slang"
[numthreads(32,1,1)]
void computeMain(uint3 id:SV_DispatchThreadID) {
    if(id.x>=cp.counts.w) return;
    CloudRayData ray=rays[id.x]; CloudIntegralData result; result.color=float4(0,0,0,1); result.statistics=0;
    if(cp.flags.x==0 || cp.cloud.x==0 || cp.cloud.y==0 || cp.cloud.z==0) { integrals[id.x]=result; return; }
    float opacity=0,moment=0;
    float3 phases=float3(cloudHG(ray.geometry.w,cp.lighting.x),cloudHG(ray.geometry.w,cp.lighting.x*.5),cloudHG(ray.geometry.w,cp.lighting.x*.25));
    [loop] for(uint k=0;k<uint(ray.direction.w);++k) {
        float begin=ray.segments[2*k],end=ray.segments[2*k+1],ds=(end-begin)/float(cp.counts.x);
        result.statistics.x+=end-begin;
        [loop] for(uint i=0;i<cp.counts.x;++i) {
            float t=begin+(float(i)+.5)*ds,h=cloudHeightAt(ray.geometry.x,ray.geometry.y,t);
            float3 position=ray.direction.xyz*t;
            float sigma=cloudDensity(ray,position,h)*cp.cloud.z;
            float sunMu=clamp(((cp.geometry.x+ray.geometry.x)*ray.geometry.z+t*ray.geometry.w)/(cp.geometry.x+h),-1,1);
            float tau=cloudSunlightOptical(ray,position,h,sunMu);
            float3 source=cp.sky.xyz;
            if(tau>=0) {
                float phase=(1-cp.lighting.z)*phases.x*exp(-tau)+cp.lighting.z*(2.0/3*phases.y*exp(-tau*.5)+1.0/3*phases.z*exp(-tau*.25));
                source+=cp.irradiance.xyz*cloudAirTransmittance(h,sunMu)*phase;
            }
            float weight=result.color.w*cloudAbsorbed(sigma*ds);
            result.color.xyz+=weight*cp.lighting.y*source;
            result.color.w*=exp(-sigma*ds); opacity+=weight; moment+=weight*t; result.statistics.z+=1;
        }
    }
    if(opacity>0) result.statistics.y=moment/opacity;
    integrals[id.x]=result;
}
