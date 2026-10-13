// Joint air/cloud transport. Surface HDR is a terminal source, never pre-fogged.
struct Parameters {
    float4 geometry, rayleigh, mie, extinction, absorption, ground, sun;
    float4 forward, right, up, sunDirection;
    uint4 frame;
    float4 depth;
};
[[vk::binding(0,0)]] ConstantBuffer<Parameters> p;
[[vk::binding(1,0)]] [[vk::image_format("rgba32f")]] RWTexture2D<float4> outputImage;
[[vk::binding(2,0)]] SamplerState linearClamp;
[[vk::binding(3,0)]] Texture2D<float4> transmittanceLut;
[[vk::binding(4,0)]] Texture2D<float4> multipleLut;
[[vk::binding(6,0)]] [[vk::image_format("rgba32f")]] RWTexture2D<float4> cloudDiagnostic;
[[vk::binding(7,0)]] [[vk::image_format("rgba32f")]] RWTexture2D<float4> mediumDiagnostic;
[[vk::binding(8,0)]] Texture2D<float> sceneDepth;
[[vk::binding(9,0)]] Texture2D<float4> surfaceColor;
#define DK_STAGE 3
#include "../../engine/render/atmosphere/shaders/AtmosphereTransport.slang"
#include "../../engine/render/clouds/shaders/CloudTypes.slang"
struct CloudFrameParameters {
    CloudParameters model;
    CloudRayData origin;
    float4 forward,right,up,clear;
};
[[vk::binding(10,0)]] ConstantBuffer<CloudFrameParameters> cloudFrame;
[[vk::binding(11,0)]] Texture2D<float2> weatherField;
[[vk::binding(12,0)]] Texture3D<float> baseField;
[[vk::binding(13,0)]] Texture3D<float> detailField;
[[vk::binding(14,0)]] SamplerState linearRepeat;
#define cp cloudFrame.model
#include "../../engine/render/clouds/shaders/CloudTransport.slang"
[numthreads(8,8,1)]
void computeMain(uint3 id:SV_DispatchThreadID) {
    if(id.x>=p.frame.x || id.y>=p.frame.y) return;
    float2 screen=(float2(id.xy)+.5)/float2(p.frame.xy)*2-1;
    float3 viewRay=p.forward.xyz+screen.x*p.right.xyz+screen.y*p.up.xyz;
    float3 direction=normalize(viewRay);
    CloudRayData ray=cloudFrame.origin;
    ray.direction.xyz=normalize(cloudFrame.forward.xyz+screen.x*cloudFrame.right.xyz+screen.y*cloudFrame.up.xyz);
    float mu=direction.y,cosine=clamp(dot(direction,p.sunDirection.xyz),-1,1);
    ray.geometry.y=mu; ray.geometry.w=cosine;
    float depth=sceneDepth.Load(int3(id.xy,0));
    float3 atmospherePath=path(p.geometry.z,mu);
    float distance=depth<1 ? p.depth.y/(depth+p.depth.x)*length(viewRay)*1000 : max(0,atmospherePath.y*1000);
    bool blocked=atmospherePath.z>0 && distance>=atmospherePath.y*1000;
    float begin=max(0,atmospherePath.x*1000),end=min(distance,atmospherePath.y*1000);
    float4 shell; bool ground; uint shellCount=cloudShell(ray.geometry.x,mu,shell,ground);
    float cuts[6]; uint count=0; cuts[count++]=begin;
    [loop] for(uint k=0;k<shellCount;++k) {
        cuts[count++]=clamp(shell[2*k],begin,max(begin,end));
        cuts[count++]=clamp(shell[2*k+1],begin,max(begin,end));
    }
    cuts[count++]=max(begin,end);
    float3 L=0,T=1,cloudL=0; float cloudT=1,opacity=0,moment=0;
    float3 phases=float3(cloudHG(cosine,cp.lighting.x),cloudHG(cosine,cp.lighting.x*.5),cloudHG(cosine,cp.lighting.x*.25));
    [loop] for(uint k=1;k<count;++k) {
        float first=cuts[k-1],last=cuts[k]; if(last<=first) continue;
        bool cloudy=false; float middle=(first+last)*.5;
        [loop] for(uint j=0;j<shellCount;++j) cloudy=cloudy || (middle>=shell[2*j] && middle<shell[2*j+1]);
        [loop] for(uint i=0;i<cp.counts.x;++i) {
            float a=lerp(first,last,cloudy ? float(i)/float(cp.counts.x) : cloudNode(i,cp.counts.x));
            float b=lerp(first,last,cloudy ? float(i+1)/float(cp.counts.x) : cloudNode(i+1,cp.counts.x));
            float ds=b-a,t=(a+b)*.5,h=cloudHeightAt(ray.geometry.x,mu,t);
            float sunMu=clamp(((cp.geometry.x+ray.geometry.x)*ray.geometry.z+t*cosine)/(cp.geometry.x+h),-1,1);
            float3 sunAir=cp.flags.y!=0 ? (path(h*.001,sunMu).z>0 ? float3(0) : cloudAirTransmittance(h,sunMu)) : float3(1);
            float3 extinction=0,airSource=0,cloudSource=0;
            if(cp.flags.y!=0) {
                float3 rayleigh,mie; medium(h*.001,rayleigh,mie,extinction);
                float rayPhase=3*(1+cosine*cosine)/(16*PI),miePhase=cloudHG(cosine,p.extinction.w);
                airSource=(rayleigh*rayPhase+mie*miePhase)*(p.frame.z!=0 ? sunAir : sunlight(h*.001,sunMu));
                if(p.up.w>0) airSource+=(rayleigh+mie)*multiple(h*.001,sunMu);
                airSource*=p.sun.rgb*.001; extinction*=.001;
            }
            float sigma=cloudy ? cloudDensity(ray,ray.direction.xyz*t,h)*cp.cloud.z : 0;
            if(cloudy) {
                float tau=cloudSunlightOptical(ray,ray.direction.xyz*t,h,sunMu);
                float3 source=cp.sky.xyz;
                if(tau>=0) {
                    float phase=(1-cp.lighting.z)*phases.x*exp(-tau)+cp.lighting.z*(2.0/3*phases.y*exp(-tau*.5)+1.0/3*phases.z*exp(-tau*.25));
                    source+=cp.irradiance.xyz*sunAir*phase;
                }
                cloudSource=sigma*cp.lighting.y*source;
            }
            extinction+=sigma;
            float3 w=T*weight(extinction,ds);
            L+=w*(airSource+cloudSource); cloudL+=w*cloudSource; T*=exp(-extinction*ds);
            float cw=cloudT*cloudAbsorbed(sigma*ds); cloudT*=exp(-sigma*ds); opacity+=cw; moment+=cw*t;
        }
    }
    float3 terminal=0;
    if(!blocked) {
        if(depth<1) terminal=surfaceColor.Load(int3(id.xy,0)).rgb;
        else {
            if(cp.flags.y==0) terminal=cloudFrame.clear.rgb;
            float3 delta=direction-p.sunDirection.xyz;
            if(p.frame.w!=0 && dot(delta,delta)<=p.sunDirection.w) terminal+=p.sun.rgb*p.sun.w;
        }
    }
    outputImage[id.xy]=float4(L+T*terminal,1);
    cloudDiagnostic[id.xy]=float4(cloudL,cloudT);
    mediumDiagnostic[id.xy]=float4(T,opacity>0 ? moment/opacity : 0);
}
