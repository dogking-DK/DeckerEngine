// Same spherical transport as the sky, with camera-to-surface finite segments.
struct Parameters {
    float4 geometry, rayleigh, mie, extinction, absorption, ground, sun;
    float4 forward, right, up, sunDirection;
    uint4 frame;
    float4 depth; // p22, p23 in km, far in km
};
[[vk::binding(0,0)]] ConstantBuffer<Parameters> p;
[[vk::binding(2,0)]] SamplerState linearClamp;
[[vk::binding(3,0)]] Texture2D<float4> transmittanceLut;
[[vk::binding(4,0)]] Texture2D<float4> multipleLut;
#define DK_STAGE 3
#include "../../engine/render/atmosphere/shaders/AtmosphereTransport.slang"
#ifdef DK_VOLUME
[[vk::binding(6,0)]] [[vk::image_format("rgba32f")]] RWTexture3D<float4> airLight;
[[vk::binding(7,0)]] [[vk::image_format("rgba32f")]] RWTexture3D<float4> airTransmission;
#else
[[vk::binding(1,0)]] [[vk::image_format("rgba32f")]] RWTexture2D<float4> outputImage;
[[vk::binding(5,0)]] Texture2D<float4> skyLut;
[[vk::binding(6,0)]] Texture3D<float4> airLight;
[[vk::binding(7,0)]] Texture3D<float4> airTransmission;
[[vk::binding(8,0)]] Texture2D<float> sceneDepth;
[[vk::binding(9,0)]] Texture2D<float4> surfaceColor;
#endif
uint segmentSteps(float h,float mu,float distance) {
    float3 segment=path(h,mu);
    float length=max(0,min(distance,segment.y)-segment.x);
    float scale=min(p.rayleigh.w,min(p.mie.w,p.ground.w));
    float3 bound=(p.rayleigh.rgb+p.extinction.rgb+p.absorption.rgb)*length;
    // Only shorten smooth, optically thin local paths well above the solar
    // horizon. Ground clipping and all long/thick/dusk segments stay unchanged.
    return p.geometry.w>.05 && length<=.05*scale && max(bound.x,max(bound.y,bound.z))<=.25 ? 8u : 128u;
}
[shader("compute")]
[numthreads(8,8,1)]
void computeMain(uint3 id:SV_DispatchThreadID) {
#ifdef DK_VOLUME
    if(id.x>=DK_AIR_SIDE || id.y>=DK_AIR_SIDE || id.z>=DK_AIR_DEPTH) return;
    float2 screen=float2(id.xy)/(DK_AIR_SIDE-1)*2-1;
    float3 ray=p.forward.xyz+screen.x*p.right.xyz+screen.y*p.up.xyz;
    float3 direction=normalize(ray),L,T;
    float distance=p.depth.z*pow(float(id.z)/(DK_AIR_DEPTH-1),2)*length(ray);
    integrateSegment(p.geometry.z,direction.y,clamp(dot(direction,p.sunDirection.xyz),-1,1),p.geometry.w,segmentSteps(p.geometry.z,direction.y,distance),false,distance,L,T);
    airLight[id]=float4(L,1); airTransmission[id]=float4(T,1);
#else
    if(id.x>=p.frame.x || id.y>=p.frame.y) return;
    float2 screen=(float2(id.xy)+.5)/float2(p.frame.xy)*2-1;
    float3 ray=p.forward.xyz+screen.x*p.right.xyz+screen.y*p.up.xyz;
    float3 direction=normalize(ray),L,T;
    float d=sceneDepth.Load(int3(id.xy,0));
    float h=p.geometry.z,mu=direction.y,cosine=clamp(dot(direction,p.sunDirection.xyz),-1,1);
    if(d<1) {
        float axial=p.depth.y/(d+p.depth.x),distance=axial*length(ray);
        // A ground-clipped voxel has a derivative discontinuity. Resolve the
        // conservative endpoint footprint explicitly instead of filtering it.
        float fraction=sqrt(saturate(axial/p.depth.z));
        float depthSpan=p.depth.z*(2*fraction/(DK_AIR_DEPTH-1)+1.0/((DK_AIR_DEPTH-1)*(DK_AIR_DEPTH-1)))*length(ray);
        float angularSpan=axial*2*(length(p.right.xyz)+length(p.up.xyz))/(DK_AIR_SIDE-1);
        bool boundary=heightAt(h,mu,distance)<2*(depthSpan+angularSpan);
        if(p.frame.z!=0 || boundary) integrateSegment(h,mu,cosine,p.geometry.w,p.frame.z!=0 ? 512 : segmentSteps(h,mu,distance),p.frame.z!=0,distance,L,T);
        else {
            float3 nodeUv=float3(screen*.5+.5,sqrt(saturate(axial/p.depth.z)));
            float3 uv=(nodeUv*float3(DK_AIR_SIDE-1,DK_AIR_SIDE-1,DK_AIR_DEPTH-1)+.5)/float3(DK_AIR_SIDE,DK_AIR_SIDE,DK_AIR_DEPTH);
            L=airLight.SampleLevel(linearClamp,uv,0).rgb;
            T=airTransmission.SampleLevel(linearClamp,uv,0).rgb;
        }
        float3 segment=path(h,mu);
        if(segment.z>0 && distance>=segment.y) T=0;
        L=L*p.sun.rgb+T*surfaceColor.Load(int3(id.xy,0)).rgb;
    } else {
        if(p.frame.z!=0) L=integrate(h,mu,cosine,p.geometry.w,512,true);
        else {
            float elevation=asin(clamp(mu,-1,1)),eh=horizon(h),row;
            if(elevation<eh) row=53*(1-sqrt(saturate((eh-elevation)/(eh+PI*.5))));
            else row=54+53*sqrt(saturate((elevation-eh)/(PI*.5-eh)));
            float phi=atan2(direction.z,direction.x);
            L=skyLut.SampleLevel(linearClamp,float2(((phi/PI*.5+.5)*191+.5)/192,(row+.5)/108),0).rgb;
        }
        L*=p.sun.rgb;
        float3 delta=direction-p.sunDirection.xyz;
        if(p.frame.w!=0 && dot(delta,delta)<=p.sunDirection.w && path(h,mu).z==0)
            L+=directTransmittance(h,mu)*p.sun.rgb*p.sun.w;
    }
    outputImage[id.xy]=float4(L,1);
#endif
}
