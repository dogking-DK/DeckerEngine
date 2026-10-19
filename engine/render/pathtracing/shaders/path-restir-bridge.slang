// Application-owned light domain. RTXDI only owns streaming/normalization math.
// Full float UVs accompany its reservoir; SDK packed UVs are not used for shading.
#include "Rtxdi/DI/Reservoir.hlsli"
[[vk::binding(10,0)]] RWStructuredBuffer<float4> restirSurfaces;
[[vk::binding(11,0)]] RWStructuredBuffer<float4> restirReservoirs;
struct RestirSample { uint light; float2 uv; float W, target, M; };
RestirSample emptyRestir() { RestirSample s={}; s.light=0xffffffffu; return s; }
float restirRandom(uint pixel,uint sample,uint candidate,uint purpose) {
    uint4 p=asuint(parameters[1]); uint h=mixBits(p.x^0x52495331u);
    h=mixBits(h^p.y); h=mixBits(h^pixel); h=mixBits(h^sample); h=mixBits(h^candidate); h=mixBits(h^purpose);
    return min((float(h>>8)+.5)/16777216.0,asfloat(0x3f7fffff));
}
void restirError() { uint ignored; InterlockedOr(status[0],1,ignored); }
#if !DK_PATH_EMPTY
float3 restirEvaluate(uint light,float2 uv,float3 position,float3 normal,PathBsdf bsdf,float3 wo,
    out float3 wi,out float3 point,out float pdf) {
    float classes=float((largest(parameters[3].xyz)>0 ? 1 : 0)+(parameters[3].w>0 ? 1 : 0));
    float3 emission; float geometry=1; point=0; wi=0; pdf=0;
    if(light==asuint(parameters[2]).z) {
        float z=1-2*uv.x,r=sqrt(max(0.0,1-z*z)),phi=2*pi*uv.y;
        wi=float3(r*cos(phi),r*sin(phi),z); emission=parameters[3].xyz; pdf=1/(classes*4*pi);
    } else {
        float4 a=triangles[light*9],b=triangles[light*9+1],c=triangles[light*9+2],n=triangles[light*9+3];
        float root=sqrt(uv.x); point=a.xyz*(1-root)+b.xyz*(root*(1-uv.y))+c.xyz*(root*uv.y);
        float3 delta=point-position; float d2=dot(delta,delta);
        pdf=(b.w/parameters[3].w)/(classes*a.w);
        if(d2==0) return 0;
        wi=delta/sqrt(d2); float cosine=-dot(normalize(n.xyz),wi); if(n.w!=0) cosine=abs(cosine);
        geometry=max(0.0,cosine)/d2; emission=triangles[light*9+5].xyz;
    }
    return evalBsdf(bsdf,normal,wo,wi).value*emission*(max(0.0,dot(normal,wi))*geometry);
}
RestirSample initialRestir(PathPayload hit,float3 position,float3 normal,PathBsdf bsdf,float3 wo,uint pixel,uint sample) {
    RestirSample chosen=emptyRestir();
    bool environment=largest(parameters[3].xyz)>0,area=parameters[3].w>0;
    if(!environment && !area) return chosen;
    RTXDI_DIReservoir reservoir=RTXDI_EmptyDIReservoir();
    uint count=asuint(parameters[32]).x,triCount=asuint(parameters[2]).z;
    for(uint i=0;i<count;++i) {
        uint light=triCount;
        if(!environment || restirRandom(pixel,sample,i,0)>=1.0/(1+(area ? 1 : 0))) {
            float target=restirRandom(pixel,sample,i,1)*parameters[3].w; uint lo=0,hi=triCount;
            while(lo<hi) { uint mid=(lo+hi)/2; if(target<triangles[mid*9+2].w) hi=mid; else lo=mid+1; }
            if(lo==triCount) { restirError(); return chosen; } light=lo;
        }
        float2 uv=float2(restirRandom(pixel,sample,i,2),restirRandom(pixel,sample,i,3));
        float3 wi,point; float pdf;
        float3 value=restirEvaluate(light,uv,position,normal,bsdf,wo,wi,point,pdf);
        float target=dot(value,float3(.2126,.7152,.0722));
        if(!all(isfinite(value)) || any(value<0) || !isfinite(pdf) || pdf<=0 || !isfinite(target/pdf)) { restirError(); return chosen; }
        if(RTXDI_StreamSample(reservoir,light,uv,restirRandom(pixel,sample,i,4),target,1/pdf)) {
            chosen.light=light; chosen.uv=uv;
        }
        if(!isfinite(reservoir.weightSum)) { restirError(); return chosen; }
    }
    RTXDI_FinalizeResampling(reservoir,1,reservoir.M);
    chosen.W=reservoir.weightSum; chosen.target=reservoir.targetPdf; chosen.M=reservoir.M;
    if(!isfinite(chosen.W) || chosen.W<0 || (chosen.target>0 && chosen.W==0)) restirError();
    return chosen;
}
float3 shadeRestir(RestirSample s,PathPayload hit,float3 position,float3 normal,PathBsdf bsdf,float3 wo,inout uint rays) {
    float3 value=0;
    if(s.light!=0xffffffffu) {
        float3 wi,point; float pdf; float3 contribution=restirEvaluate(s.light,s.uv,position,normal,bsdf,wo,wi,point,pdf);
        uint emitter=s.light==asuint(parameters[2]).z ? 0xffffffffu : s.light;
        if(visible(hit.index,position,wi,emitter,point,rays)) value=contribution*s.W;
    }
    if(asuint(parameters[2]).y!=0) {
        float3 wi=normalize(parameters[5].xyz); float cosine=max(0.0,dot(normal,wi));
        if(cosine>0 && visible(hit.index,position,wi,0xffffffffu,0,rays))
            value+=evalBsdf(bsdf,normal,wo,wi).value*parameters[4].xyz*cosine;
    }
    return value;
}
#endif
void storeRestir(uint pixel,RestirSample s,uint epoch) {
    restirReservoirs[pixel*2]=float4(asfloat(s.light),s.uv,s.W);
    restirReservoirs[pixel*2+1]=float4(s.target,s.M,asfloat(epoch),0);
}
RestirSample loadRestir(uint pixel,uint epoch) {
    float4 a=restirReservoirs[pixel*2],b=restirReservoirs[pixel*2+1];
    if(asuint(b.z)!=epoch) restirError();
    RestirSample s; s.light=asuint(a.x); s.uv=a.yz; s.W=a.w; s.target=b.x; s.M=b.y; return s;
}
