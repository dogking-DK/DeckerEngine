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
static const float PI=3.14159265358979323846;
float2 roots(float h,float mu,float boundary) {
    float b=(p.geometry.x+h)*mu;
    float c=(h-boundary)*(2*p.geometry.x+h+boundary);
    float discriminant=b*b-c;
    if(discriminant<0) return float2(1,-1);
    float q=-b-(b>=0 ? 1 : -1)*sqrt(discriminant);
    if(q==0) return 0;
    float other=c/q;
    return float2(min(q,other),max(q,other));
}
float3 path(float h,float mu) {
    float2 top=roots(h,mu,p.geometry.y);
    if(top.y<0 || top.x>top.y) return 0;
    float3 result=float3(max(0,top.x),top.y,0);
    float2 groundHit=roots(h,mu,0);
    if(mu<0 && groundHit.x<=groundHit.y && groundHit.x>=0 && groundHit.x<=result.y) { result.y=groundHit.x; result.z=1; }
    return result;
}
float heightAt(float h,float mu,float t) {
    float r=p.geometry.x+h, delta=t*(t+2*r*mu);
    return max(0,h+delta/(sqrt(max(0,r*r+delta))+r));
}
float node(uint i,uint count) { return .5-.5*cos(PI*float(i)/float(count)); }
void medium(float h,out float3 ray,out float3 mie,out float3 ext) {
    ray=p.rayleigh.rgb*exp(-h/p.rayleigh.w);
    mie=p.mie.rgb*exp(-h/p.mie.w);
    ext=ray+p.extinction.rgb*exp(-h/p.mie.w)+p.absorption.rgb*max(0,1-abs(h-p.absorption.w)/p.ground.w);
}
float3 weight(float3 ext,float ds) {
    float3 optical=ext*ds;
    // Avoid cancellation and 0/0 in vacuum (including eager vector selection).
    return ds*select(optical<.001,1-optical*.5+optical*optical/6,(1-exp(-optical))/max(optical,1e-20));
}
float3 opticalTransmittance(float h,float mu,float begin,float end,uint count) {
    float3 optical=0;
    [loop] for(uint i=0;i<count;++i) {
        float a=lerp(begin,end,node(i,count)),b=lerp(begin,end,node(i+1,count));
        float3 ray,mie,ext; medium(heightAt(h,mu,(a+b)*.5),ray,mie,ext);
        optical+=ext*(b-a);
    }
    return exp(-optical);
}
float3 directTransmittance(float h,float mu) {
    float3 segment=path(h,mu);
    if(segment.z>0) return 0;
    return opticalTransmittance(h,mu,segment.x,segment.y,512);
}
float2 endpointUv(float2 uv,float2 size) { return (saturate(uv)*(size-1)+.5)/size; }
#if DK_STAGE > 0
float3 sunlight(float h,float mu) {
    float3 segment=path(h,mu);
    if(segment.z>0) return 0;
    float H=sqrt(p.geometry.y*(2*p.geometry.x+p.geometry.y));
    float rho=sqrt(max(0,h*(2*p.geometry.x+h)));
    float dmin=p.geometry.y-h, dmax=rho+H;
    float2 uv=float2((segment.y-dmin)/max(dmax-dmin,1e-8),rho/H);
    return transmittanceLut.SampleLevel(linearClamp,endpointUv(uv,float2(256,64)),0).rgb;
}
#endif
#if DK_STAGE > 1
float3 multiple(float h,float mu) {
    return multipleLut.SampleLevel(linearClamp,endpointUv(float2(mu*.5+.5,sqrt(saturate(h/p.geometry.y))),float2(32,32)),0).rgb;
}
float3 integrate(float h,float mu,float cosine,float sunMu,uint count,bool reference) {
    float3 segment=path(h,mu),throughput=1,radiance=0;
    float rayPhase=3*(1+cosine*cosine)/(16*PI);
    float g=p.extinction.w;
    float miePhase=(1-g*g)/(4*PI*pow(1+g*g-2*g*cosine,1.5));
    [loop] for(uint i=0;i<count;++i) {
        float a=lerp(segment.x,segment.y,node(i,count)),b=lerp(segment.x,segment.y,node(i+1,count)),t=(a+b)*.5;
        float sh=heightAt(h,mu,t);
        float sm=clamp(((p.geometry.x+h)*sunMu+t*cosine)/(p.geometry.x+sh),-1,1);
        float3 ray,mie,ext; medium(sh,ray,mie,ext);
        float3 source=(ray*rayPhase+mie*miePhase)*(reference ? directTransmittance(sh,sm) : sunlight(sh,sm));
        if(p.up.w>0) source+=(ray+mie)*multiple(sh,sm);
        radiance+=throughput*source*weight(ext,b-a);
        throughput*=exp(-ext*(b-a));
    }
    return radiance;
}
float horizon(float h) { return asin(-sqrt(h*(2*p.geometry.x+h))/(p.geometry.x+h)); }
#endif
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
