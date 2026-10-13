// Independently implemented spherical transport. Distances: km, coefficients: km^-1.
struct Parameters {
    float4 geometry; // R, H, observer height, local sun mu
    float4 rayleigh; // RGB coefficient, scale height
    float4 mie; // RGB scattering, scale height
    float4 extinction; // RGB Mie extinction, g
    float4 absorption; // RGB coefficient, center height
    float4 ground; // RGB albedo, absorption half width
    float4 sun; // RGB irradiance, disk radiance multiplier (CPU double sine)
    float4 forward; // local XYZ, tan(fov/2)
    float4 right; // local XYZ, aspect
    float4 up; // local XYZ, MS enabled
    float4 sunDirection; // Local XYZ, squared chord radius for narrow solar disks.
    uint4 frame; // width, height, reference, sun disk (enabled bit 1)
};
[[vk::binding(0,0)]] ConstantBuffer<Parameters> p;
[[vk::binding(1,0)]] [[vk::image_format("rgba32f")]] RWTexture2D<float4> outputImage;
#if DK_STAGE > 0
[[vk::binding(2,0)]] SamplerState linearClamp;
[[vk::binding(3,0)]] Texture2D<float4> transmittanceLut;
#endif
#if DK_STAGE > 1
[[vk::binding(4,0)]] Texture2D<float4> multipleLut;
#endif
#if DK_STAGE > 2
[[vk::binding(5,0)]] Texture2D<float4> skyLut;
#endif
#include "AtmosphereTransport.slang"
[shader("compute")]
[numthreads(8,8,1)]
void computeMain(uint3 id:SV_DispatchThreadID) {
#if DK_STAGE == 0
    if(id.x>=256 || id.y>=64) return;
    float H=sqrt(p.geometry.y*(2*p.geometry.x+p.geometry.y)),rho=H*float(id.y)/63;
    float h=rho*rho/(sqrt(p.geometry.x*p.geometry.x+rho*rho)+p.geometry.x);
    float d=lerp(p.geometry.y-h,rho+H,float(id.x)/255);
    float mu=d>1e-6 ? clamp((H*H-rho*rho-d*d)/(2*(p.geometry.x+h)*d),-1,1) : 1;
    outputImage[id.xy]=float4(opticalTransmittance(h,mu,0,d,512),1);
#elif DK_STAGE == 1
    if(id.x>=32 || id.y>=32) return;
    float h=p.geometry.y*pow(float(id.y)/31,2),sunMu=float(id.x)/31*2-1;
    float3 radiance=0,feedback=0;
    [loop] for(uint y=0;y<32;++y) [loop] for(uint x=0;x<32;++x) {
        float mu=2*(float(y)+.5)/32-1,phi=2*PI*(float(x)+.5)/32;
        float cosine=mu*sunMu+sqrt((1-mu*mu)*(1-sunMu*sunMu))*cos(phi);
        float3 segment=path(h,mu),throughput=1;
        [loop] for(uint i=0;i<64;++i) {
            float a=segment.y*node(i,64),b=segment.y*node(i+1,64),t=(a+b)*.5;
            float sh=heightAt(h,mu,t),sm=clamp(((p.geometry.x+h)*sunMu+t*cosine)/(p.geometry.x+sh),-1,1);
            float3 ray,mie,ext; medium(sh,ray,mie,ext);
            float3 w=throughput*weight(ext,b-a),scattering=ray+mie;
            radiance+=w*scattering*sunlight(sh,sm)/(4*PI);
            feedback+=w*scattering;
            throughput*=exp(-ext*(b-a));
        }
        if(segment.z>0) {
            float gm=clamp(((p.geometry.x+h)*sunMu+segment.y*cosine)/p.geometry.x,-1,1);
            radiance+=throughput*p.ground.rgb*sunlight(0,gm)*max(0,gm)/PI;
        }
    }
    radiance/=1024; feedback/=1024;
    outputImage[id.xy]=float4(radiance/(1-feedback),max(feedback.r,max(feedback.g,feedback.b)));
#elif DK_STAGE == 2
    if(id.x>=192 || id.y>=108) return;
    float h=p.geometry.z,eh=horizon(h),elevation;
    if(id.y<54) elevation=eh-(eh+PI*.5)*pow(1-float(id.y)/53,2)-1e-6;
    else elevation=eh+(PI*.5-eh)*pow(float(id.y-54)/53,2)+1e-6;
    elevation=clamp(elevation,-PI*.5,PI*.5);
    float mu=sin(elevation),phi=2*PI*float(id.x)/191-PI,sunMu=p.geometry.w;
    float cosine=clamp(mu*sunMu+sqrt(max(0,(1-mu*mu)*(1-sunMu*sunMu)))*cos(phi),-1,1);
    outputImage[id.xy]=float4(integrate(h,mu,cosine,sunMu,128,false),1);
#else
    if(id.x>=p.frame.x || id.y>=p.frame.y) return;
    float2 screen=(float2(id.xy)+.5)/float2(p.frame.xy)*2-1;
    float3 direction=normalize(p.forward.xyz+p.right.xyz*screen.x*p.forward.w*p.right.w-p.up.xyz*screen.y*p.forward.w);
    float h=p.geometry.z,mu=direction.y,sunMu=p.geometry.w;
    float cosine=clamp(direction.x*sqrt(max(0,1-sunMu*sunMu))+mu*sunMu,-1,1);
    float3 radiance=0;
    if((p.frame.w&2)!=0) {
        if(p.frame.z!=0) radiance=integrate(h,mu,cosine,sunMu,512,true);
        else {
            float elevation=asin(clamp(mu,-1,1)),eh=horizon(h),row;
            if(elevation<eh) row=53*(1-sqrt(saturate((eh-elevation)/(eh+PI*.5))));
            else row=54+53*sqrt(saturate((elevation-eh)/(PI*.5-eh)));
            float phi=atan2(direction.z,direction.x);
            float2 uv=float2(((phi/PI*.5+.5)*191+.5)/192,(row+.5)/108);
            radiance=skyLut.SampleLevel(linearClamp,uv,0).rgb;
        }
        radiance*=p.sun.rgb;
        float3 sunDelta=direction-p.sunDirection.xyz;
        if((p.frame.w&1)!=0 && dot(sunDelta,sunDelta)<=p.sunDirection.w) {
            float3 segment=path(h,mu);
            if(segment.z==0) radiance+=directTransmittance(h,mu)*p.sun.rgb*p.sun.w;
        }
    }
    outputImage[id.xy]=float4(radiance,1);
#endif
}
