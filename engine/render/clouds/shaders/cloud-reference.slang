// No jitter, history, early termination or empty-space skipping. Units: metres.
struct Parameters {
    float4 geometry; // R, atmosphere H, cloud bottom, cloud top
    float4 cloud; // coverage, density, extinction, homogeneous density
    float4 scales; // weather, base, detail, bottom fade
    float4 lighting; // g, albedo, multiple-scattering redistribution, top fade
    float4 sun, irradiance, sky;
    float4 rayleigh, mie, absorption, air;
    uint4 counts; // primary, sunlight, atmosphere, ray count
    uint4 flags; // clouds enabled, atmosphere enabled, self shadow, homogeneous
};
struct Ray { float4 geometry,direction,weather,base,detail,segments; };
struct Integral { float4 color,statistics; };
[[vk::binding(0,0)]] ConstantBuffer<Parameters> p;
[[vk::binding(1,0)]] StructuredBuffer<Ray> rays;
[[vk::binding(2,0)]] RWStructuredBuffer<Integral> integrals;
[[vk::binding(3,0)]] Texture2D<float2> weatherField;
[[vk::binding(4,0)]] Texture3D<float> baseField;
[[vk::binding(5,0)]] Texture3D<float> detailField;
[[vk::binding(6,0)]] SamplerState linearRepeat;
static const float PI=3.14159265358979323846;
float2 roots(float h,float mu,float boundary) {
    float b=(p.geometry.x+h)*mu,c=(h-boundary)*(2*p.geometry.x+h+boundary),d=b*b-c;
    if(d<=0) return float2(1,-1);
    float q=-b-(b>=0 ? 1 : -1)*sqrt(d),other=c/q;
    return float2(min(q,other),max(q,other));
}
float heightAt(float h,float mu,float t) {
    float r=p.geometry.x+h,delta=t*(t+2*r*mu);
    return h+delta/(sqrt(max(0,r*r+delta))+r);
}
float smooth01(float x) { x=saturate(x); return x*x*(3-2*x); }
float density(Ray ray,float3 displacement,float height) {
    if(p.flags.w!=0) return p.cloud.y*p.cloud.w;
    if(height<=p.geometry.z || height>=p.geometry.w) return 0;
    float2 weather=weatherField.SampleLevel(linearRepeat,ray.weather.xy+displacement.xz/p.scales.x,0);
    float base=baseField.SampleLevel(linearRepeat,ray.base.xyz+displacement/p.scales.y,0);
    float detail=detailField.SampleLevel(linearRepeat,ray.detail.xyz+displacement/p.scales.z,0);
    float coverage=saturate((weather.x-(1-p.cloud.x))/p.cloud.x);
    float h=(height-p.geometry.z)/(p.geometry.w-p.geometry.z);
    float profile=smooth01(h/p.scales.w)*smooth01((.6+.4*weather.y-h)/p.lighting.w);
    return p.cloud.y*saturate((base*profile-(1-coverage))/.35-.2*detail);
}
// Up to two shell segments. Ground occultation is independent of the cloud shell.
uint shell(float h,float mu,out float4 segments,out bool ground) {
    segments=0; ground=false; float limit=2*(p.geometry.x+p.geometry.w);
    float2 hit=roots(h,mu,0);
    if(mu<0 && hit.x>=0 && hit.x<=hit.y && hit.x<=limit) { limit=hit.x; ground=true; }
    float2 outer=roots(h,mu,p.geometry.w);
    float begin=max(0,outer.x),end=min(limit,outer.y);
    if(end<=begin) return 0;
    float2 inner=roots(h,mu,p.geometry.z);
    float a=max(begin,inner.x),b=min(end,inner.y);
    if(b<=a) { segments.xy=float2(begin,end); return 1; }
    uint count=0;
    if(a>begin) { segments.xy=float2(begin,a); count=1; }
    if(end>b) { if(count==0) segments.xy=float2(b,end); else segments.zw=float2(b,end); ++count; }
    return count;
}
float sunlightOptical(Ray ray,float3 position,float h,float mu) {
    float4 segments; bool ground; uint count=shell(h,mu,segments,ground);
    if(ground) return -1;
    if(p.flags.z==0) return 0;
    float optical=0;
    [loop] for(uint k=0;k<count;++k) {
        float a=segments[2*k],b=segments[2*k+1],ds=(b-a)/float(p.counts.y);
        [loop] for(uint j=0;j<p.counts.y;++j) {
            float t=a+(float(j)+.5)*ds;
            optical+=density(ray,position+p.sun.xyz*t,heightAt(h,mu,t))*p.cloud.z*ds;
        }
    }
    return optical;
}
float node(uint i,uint count) { return .5-.5*cos(PI*float(i)/float(count)); }
float3 airTransmittance(float h,float mu) {
    if(p.flags.y==0) return 1;
    float2 top=roots(h,mu,p.geometry.y);
    if(top.x>top.y || top.y<0) return 1;
    float begin=max(0,top.x),end=top.y;
    float3 optical=0;
    [loop] for(uint i=0;i<p.counts.z;++i) {
        float a=lerp(begin,end,node(i,p.counts.z)),b=lerp(begin,end,node(i+1,p.counts.z));
        float height=max(0,heightAt(h,mu,(a+b)*.5));
        float3 extinction=p.rayleigh.xyz*exp(-height/p.rayleigh.w)+p.mie.xyz*exp(-height/p.mie.w)
            +p.absorption.xyz*max(0,1-abs(height-p.absorption.w)/p.air.x);
        optical+=extinction*(b-a);
    }
    return exp(-optical);
}
float hg(float cosine,float g) { return (1-g*g)/(4*PI*pow(1+g*g-2*g*cosine,1.5)); }
float absorbed(float optical) {
    return optical<.001 ? optical*(1-optical*.5+optical*optical/6) : 1-exp(-optical);
}
[numthreads(32,1,1)]
void computeMain(uint3 id:SV_DispatchThreadID) {
    if(id.x>=p.counts.w) return;
    Ray ray=rays[id.x]; Integral result; result.color=float4(0,0,0,1); result.statistics=0;
    if(p.flags.x==0 || p.cloud.x==0 || p.cloud.y==0 || p.cloud.z==0) { integrals[id.x]=result; return; }
    float opacity=0,moment=0;
    float3 phases=float3(hg(ray.geometry.w,p.lighting.x),hg(ray.geometry.w,p.lighting.x*.5),hg(ray.geometry.w,p.lighting.x*.25));
    [loop] for(uint k=0;k<uint(ray.direction.w);++k) {
        float begin=ray.segments[2*k],end=ray.segments[2*k+1],ds=(end-begin)/float(p.counts.x);
        result.statistics.x+=end-begin;
        [loop] for(uint i=0;i<p.counts.x;++i) {
            float t=begin+(float(i)+.5)*ds,h=heightAt(ray.geometry.x,ray.geometry.y,t);
            float3 position=ray.direction.xyz*t;
            float sigma=density(ray,position,h)*p.cloud.z;
            float sunMu=clamp(((p.geometry.x+ray.geometry.x)*ray.geometry.z+t*ray.geometry.w)/(p.geometry.x+h),-1,1);
            float tau=sunlightOptical(ray,position,h,sunMu);
            float3 source=p.sky.xyz;
            if(tau>=0) {
                float phase=(1-p.lighting.z)*phases.x*exp(-tau)+p.lighting.z*(2.0/3*phases.y*exp(-tau*.5)+1.0/3*phases.z*exp(-tau*.25));
                source+=p.irradiance.xyz*airTransmittance(h,sunMu)*phase;
            }
            float weight=result.color.w*absorbed(sigma*ds);
            result.color.xyz+=weight*p.lighting.y*source;
            result.color.w*=exp(-sigma*ds); opacity+=weight; moment+=weight*t; result.statistics.z+=1;
        }
    }
    if(opacity>0) result.statistics.y=moment/opacity;
    integrals[id.x]=result;
}
