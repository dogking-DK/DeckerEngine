// Application-owned light domain. RTXDI only owns streaming/normalization math.
// Full float UVs accompany its reservoir; SDK packed UVs are not used for shading.
#include "Rtxdi/DI/Reservoir.hlsli"
[[vk::binding(10,0)]] RWStructuredBuffer<float4> restirSurfaces;
[[vk::binding(11,0)]] RWStructuredBuffer<float4> restirReservoirs;
[[vk::binding(12,0)]] StructuredBuffer<float4> previousSurfaces;
[[vk::binding(13,0)]] StructuredBuffer<float4> previousReservoirs;
[[vk::binding(14,0)]] StructuredBuffer<float4> spatialReservoirs;
struct RestirSample { uint light; float2 uv; float W, target, M; uint age; };
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
        uint before=rays;
        if(visible(hit.index,position,wi,emitter,point,rays)) value=contribution*s.W;
        if(asuint(parameters[40]).w!=0) { uint ignored; InterlockedAdd(status[13],rays-before,ignored); }
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
    restirReservoirs[pixel*2+1]=float4(s.target,s.M,asfloat(epoch),asfloat(s.age));
}
RestirSample loadRestir(uint pixel,uint epoch) {
    float4 a=restirReservoirs[pixel*2],b=restirReservoirs[pixel*2+1];
    if(asuint(b.z)!=epoch) restirError();
    RestirSample s; s.light=asuint(a.x); s.uv=a.yz; s.W=a.w; s.target=b.x; s.M=b.y; s.age=asuint(b.w); return s;
}
void temporalStat(uint at) { uint ignored; InterlockedAdd(status[at],1,ignored); }
float4 previousClip(float3 p) {
    float4 v=float4(p,1);
    return float4(dot(parameters[35],v),dot(parameters[36],v),dot(parameters[37],v),dot(parameters[38],v));
}
bool temporalReceiver(uint pixel,out uint previousPixel) {
    previousPixel=0; float4 hit=restirSurfaces[pixel*4];
    if(hit.w==0 || asuint(parameters[33]).z==0) return false;
    float3 position=restirSurfaces[pixel*4+2].xyz;
    float4 clip=previousClip(position);
    if(!all(isfinite(clip)) || clip.w<=0 || clip.z<0 || clip.z>clip.w) return false;
    float2 uv=clip.xy/clip.w*.5+.5;
    if(any(uv<0) || any(uv>=1)) return false;
    uint2 size=asuint(parameters[39]).xy,xy=uint2(uv*float2(size));
    previousPixel=xy.y*size.x+xy.x;
    float4 oldHit=previousSurfaces[previousPixel*4];
    if(oldHit.w!=hit.w) return false; // Primitive also fixes material identity in the frozen scene.
    float3 oldPosition=previousSurfaces[previousPixel*4+2].xyz;
    if(distance(oldPosition,position)>parameters[34].x ||
        dot(previousSurfaces[previousPixel*4+1].xyz,restirSurfaces[pixel*4+1].xyz)<parameters[34].y) return false;
    if(asuint(previousSurfaces[previousPixel*4+3].w)!=asuint(parameters[34]).w) { restirError(); return false; }
    return true;
}
#if !DK_PATH_EMPTY
float temporalTarget(RestirSample s,float4 hit,float3 position,float3 normal,float3 wo) {
    if(s.light==0xffffffffu) return 0;
    float3 wi,point; float pdf;
    float3 value=restirEvaluate(s.light,s.uv,position,normal,materialAt(uint(hit.w)-1,hit.yz),wo,wi,point,pdf);
    float result=dot(value,float3(.2126,.7152,.0722));
    if(!isfinite(result) || result<0) restirError();
    return result;
}
RTXDI_DIReservoir sdkReservoir(RestirSample s,float M) {
    RTXDI_DIReservoir r=RTXDI_EmptyDIReservoir();
    r.M=M; r.weightSum=s.W; r.targetPdf=s.target;
    if(s.light!=0xffffffffu) r.lightData=s.light|RTXDI_DIReservoir_LightValidBit;
    return r;
}
#endif
void temporalRestir(uint pixel,uint epoch) {
    RestirSample fresh=loadRestir(pixel,epoch),result=fresh;
    uint oldPixel;
    if(temporalReceiver(pixel,oldPixel)) {
        float4 a=previousReservoirs[oldPixel*2],b=previousReservoirs[oldPixel*2+1];
        if(asuint(b.z)!=asuint(parameters[34]).w) restirError();
        if(asuint(b.w)>=asuint(parameters[33]).w) temporalStat(4);
        else {
#if !DK_PATH_EMPTY
            RestirSample old; old.light=asuint(a.x); old.uv=a.yz; old.W=a.w; old.target=b.x; old.M=min(b.y,float(asuint(parameters[34]).z)); old.age=asuint(b.w);
            float4 hit=restirSurfaces[pixel*4];
            float3 position=restirSurfaces[pixel*4+2].xyz,normal=restirSurfaces[pixel*4+1].xyz,wo=restirSurfaces[pixel*4+3].xyz;
            RTXDI_DIReservoir combined=RTXDI_EmptyDIReservoir();
            RTXDI_CombineDIReservoirs(combined,sdkReservoir(fresh,fresh.M),0,fresh.target);
            float target=temporalTarget(old,hit,position,normal,wo);
            if(RTXDI_CombineDIReservoirs(combined,sdkReservoir(old,old.M),restirRandom(pixel,epoch,0,5),target)) result=old;
            float support=temporalTarget(result,previousSurfaces[oldPixel*4],previousSurfaces[oldPixel*4+2].xyz,
                previousSurfaces[oldPixel*4+1].xyz,previousSurfaces[oldPixel*4+3].xyz)>0 ? old.M : 0;
            RTXDI_FinalizeResampling(combined,1,fresh.M+support);
            result.W=combined.weightSum; result.target=combined.targetPdf; result.M=combined.M;
            result.age=old.age+1; // Ancestry of ALL sources, never the age of the selected sample.
            if(combined.weightSum==0) result.light=0xffffffffu;
            temporalStat(2); if(support==0 && old.M>0) temporalStat(7);
#endif
        }
    } else temporalStat(3);
    if(!isfinite(result.W) || result.W<0 || !isfinite(result.M) || (result.target>0 && result.W==0)) restirError();
    uint ignored; InterlockedMax(status[5],result.age,ignored); InterlockedMax(status[6],uint(result.M),ignored);
    storeRestir(pixel,result,epoch);
}
RestirSample loadSpatial(uint pixel,uint epoch) {
    float4 a=spatialReservoirs[pixel*2],b=spatialReservoirs[pixel*2+1];
    if(asuint(b.z)!=epoch) restirError();
    RestirSample s; s.light=asuint(a.x); s.uv=a.yz; s.W=a.w; s.target=b.x;
    s.M=min(b.y,float(asuint(parameters[40]).z)); s.age=asuint(b.w); return s;
}
void spatialRestir(uint pixel,uint epoch) {
    RestirSample own=loadSpatial(pixel,epoch),result=own;
#if !DK_PATH_EMPTY
    float4 hit=restirSurfaces[pixel*4];
    if(hit.w!=0) {
        float3 position=restirSurfaces[pixel*4+2].xyz,normal=restirSurfaces[pixel*4+1].xyz,wo=restirSurfaces[pixel*4+3].xyz;
        uint4 config=asuint(parameters[40]); uint2 size=asuint(parameters[39]).xy;
        uint pixels[9]; float counts[9]; pixels[0]=pixel; counts[0]=own.M; uint count=1,age=own.age;
        RTXDI_DIReservoir combined=RTXDI_EmptyDIReservoir();
        RTXDI_CombineDIReservoirs(combined,sdkReservoir(own,own.M),0,own.target);
        uint side=2*config.y+1,center=(side*side)/2;
        for(uint i=0;i<config.x;++i) {
            uint offset=min(uint(restirRandom(pixel,epoch,i,6)*float(side*side-1)),side*side-2);
            if(offset>=center) ++offset;
            int2 xy=int2(pixel%size.x,pixel/size.x)+int2(offset%side,offset/side)-int(config.y);
            bool accepted=all(xy>=0) && all(xy<int2(size));
            uint source=accepted ? uint(xy.y)*size.x+uint(xy.x) : 0;
            if(accepted) {
                for(uint j=0;j<count;++j) if(pixels[j]==source) accepted=false;
                float4 sourceHit=restirSurfaces[source*4];
                if(sourceHit.w==0 || distance(position,restirSurfaces[source*4+2].xyz)>parameters[41].x ||
                    dot(normal,restirSurfaces[source*4+1].xyz)<parameters[41].y) accepted=false;
            }
            if(!accepted) { temporalStat(9); continue; }
            if(asuint(restirSurfaces[source*4+3].w)!=epoch) restirError();
            RestirSample s=loadSpatial(source,epoch);
            pixels[count]=source; counts[count]=s.M; ++count; age=max(age,s.age);
            float target=temporalTarget(s,hit,position,normal,wo);
            if(RTXDI_CombineDIReservoirs(combined,sdkReservoir(s,s.M),restirRandom(pixel,epoch,i,7),target)) result=s;
            temporalStat(8);
        }
        float support=0;
        for(uint j=0;j<count;++j) {
            uint source=pixels[j];
            if(temporalTarget(result,restirSurfaces[source*4],restirSurfaces[source*4+2].xyz,
                restirSurfaces[source*4+1].xyz,restirSurfaces[source*4+3].xyz)>0) support+=counts[j];
        }
        RTXDI_FinalizeResampling(combined,1,support);
        result.W=combined.weightSum; result.target=combined.targetPdf; result.M=combined.M; result.age=age;
        if(result.W==0) result.light=0xffffffffu;
        if(support<combined.M && result.W>0) temporalStat(10);
    }
#endif
    if(!isfinite(result.W) || result.W<0 || !isfinite(result.M) || (result.target>0 && result.W==0)) restirError();
    uint ignored; InterlockedMax(status[11],uint(result.M),ignored); InterlockedMax(status[12],result.age,ignored);
    storeRestir(pixel,result,epoch);
}
