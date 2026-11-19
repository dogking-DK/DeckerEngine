// Surface layout v2: hit/normal/position/wo+epoch, then integer key and signed cell.
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
bool paperRestir() { return asuint(parameters[32]).z==2; }
float restirDepth(float3 position) { return dot(parameters[43],float4(position,1)); }
bool cachedRestirVisibility() { return paperRestir() && asuint(parameters[32]).y!=2; }
// Visibility is evaluated in the current frozen scene, including old receivers.
// It is not a cache of last frame's shadow result.
bool restirVisible(RestirSample s,PathPayload hit,float3 position,float3 wi,float3 point,uint pixel) {
#if !DK_PATH_EMPTY
    uint rays=0; bool result=visible(hit,position,wi,s.light==asuint(parameters[2]).z ? 0xffffffffu : s.light,point,rays);
    restirStat(pixel,13,rays); return result;
#else
    return true;
#endif
}
RestirSample zeroRestir(RestirSample s) { s.light=0xffffffffu; s.target=0; s.W=0; return s; }
float restirRandom(uint pixel,uint sample,uint candidate,uint purpose) {
    uint4 p=asuint(parameters[1]); uint h=mixBits(p.x^0x52495331u);
    h=mixBits(h^p.y); h=mixBits(h^pixel); h=mixBits(h^sample); h=mixBits(h^candidate); h=mixBits(h^purpose);
    return min((float(h>>8)+.5)/16777216.0,asfloat(0x3f7fffff));
}
void restirError() { uint ignored; InterlockedOr(status[0],1,ignored); }
// Match the renderer's 256 x 16-word diagnostic storage. Error/medium budget
// words remain global; only observational words 2..15 are sharded.
uint restirStatIndex(uint pixel,uint at) { return (mixBits(pixel)&255u)*16u+at; }
void restirStat(uint pixel,uint at,uint value=1) {
    uint ignored; InterlockedAdd(status[restirStatIndex(pixel,at)],value,ignored);
}
void restirMax(uint pixel,uint at,uint value) {
    uint ignored; InterlockedMax(status[restirStatIndex(pixel,at)],value,ignored);
}
float3 restirEvaluate(uint light,float2 uv,float3 position,float3 normal,PathBsdf bsdf,float3 wo,
    out float3 wi,out float3 point,out float pdf) {
    bool medium=asuint(parameters[12]).x!=0;
    float classes=float((largest(parameters[3].xyz)>0 ? 1 : 0)+(parameters[3].w>0 ? 1 : 0)+(medium && asuint(parameters[2]).y!=0 ? 1 : 0));
    float3 emission; float geometry=1; point=0; wi=0; pdf=0;
    if(light==asuint(parameters[2]).z) {
        float z=1-2*uv.x,r=sqrt(max(0.0,1-z*z)),phi=2*pi*uv.y;
        wi=medium ? airDirection(float3(0,1,0),z,uv.y) : float3(r*cos(phi),r*sin(phi),z);
        emission=parameters[3].xyz; pdf=1/(classes*4*pi);
    } else if(medium && light==asuint(parameters[2]).z+1) {
        wi=airSunDirection(uv.x,uv.y); emission=parameters[4].xyz; pdf=parameters[21].y/classes;
    } else {
#if !DK_PATH_EMPTY
        float4 a=triangles[light*9],b=triangles[light*9+1],c=triangles[light*9+2],n=triangles[light*9+3];
        float root=sqrt(uv.x); point=a.xyz*(1-root)+b.xyz*(root*(1-uv.y))+c.xyz*(root*uv.y);
        float3 delta=point-position; float d2=dot(delta,delta);
        pdf=(b.w/parameters[3].w)/(classes*a.w);
        if(d2==0) return 0;
        wi=delta/sqrt(d2); float cosine=-dot(normalize(n.xyz),wi); if(n.w!=0) cosine=abs(cosine);
        geometry=max(0.0,cosine)/d2; emission=triangles[light*9+5].xyz;
#else
        restirError(); return 0;
#endif
    }
    return evalBsdf(bsdf,normal,wo,wi).value*emission*(max(0.0,dot(normal,wi))*geometry);
}
RestirSample initialRestir(PathPayload hit,float3 position,float3 normal,PathBsdf bsdf,float3 wo,uint pixel,uint sample) {
    RestirSample chosen=emptyRestir();
    bool environment=largest(parameters[3].xyz)>0,area=parameters[3].w>0;
    bool sun=asuint(parameters[12]).x!=0 && asuint(parameters[2]).y!=0;
    uint classes=(environment ? 1u : 0u)+(sun ? 1u : 0u)+(area ? 1u : 0u);
    if(classes==0) return chosen;
    RTXDI_DIReservoir reservoir=RTXDI_EmptyDIReservoir();
    uint count=asuint(parameters[32]).x,triCount=asuint(parameters[2]).z;
    for(uint i=0;i<count;++i) {
        uint light=triCount;
        int selected=int(restirRandom(pixel,sample,i,0)*float(classes));
        if(environment && selected--==0) light=triCount;
        else if(sun && selected--==0) light=triCount+1;
        else {
#if !DK_PATH_EMPTY
            float target=restirRandom(pixel,sample,i,1)*parameters[3].w; uint lo=0,hi=triCount;
            while(lo<hi) { uint mid=(lo+hi)/2; if(target<triangles[mid*9+2].w) hi=mid; else lo=mid+1; }
            if(lo==triCount) { restirError(); return chosen; } light=lo;
#else
            restirError(); return chosen;
#endif
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
    if(cachedRestirVisibility() && chosen.light!=0xffffffffu) {
        float3 wi,point; float pdf;
        restirEvaluate(chosen.light,chosen.uv,position,normal,bsdf,wo,wi,point,pdf);
        if(!restirVisible(chosen,hit,position,wi,point,pixel)) chosen=zeroRestir(chosen); // Keep M, including zero draws.
    }
    return chosen;
}
#if !DK_PATH_EMPTY
float3 shadeRestir(RestirSample s,PathPayload hit,float3 position,float3 normal,PathBsdf bsdf,float3 wo,uint pixel,inout uint rays
#if DK_PATH_SIGNALS
    ,out float3 diffuse
#endif
) {
    float3 value=0;
#if DK_PATH_SIGNALS
    diffuse=0;
#endif
    if(s.light!=0xffffffffu) {
        float3 wi,point; float pdf; float3 contribution=restirEvaluate(s.light,s.uv,position,normal,bsdf,wo,wi,point,pdf);
        uint emitter=s.light==asuint(parameters[2]).z ? 0xffffffffu : s.light;
        uint before=rays;
        if(cachedRestirVisibility() || visible(hit,position,wi,emitter,point,rays)) value=contribution*s.W;
#if DK_PATH_SIGNALS
        diffuse=value*diffuseFraction(bsdf,normal,wo,wi);
#endif
        if(asuint(parameters[40]).w!=0) restirStat(pixel,13,rays-before);
    }
    if(asuint(parameters[2]).y!=0) {
        float3 wi=normalize(parameters[5].xyz); float cosine=max(0.0,dot(normal,wi));
        if(cosine>0 && visible(hit,position,wi,0xffffffffu,0,rays)) {
#if DK_PATH_SIGNALS
            diffuse+=evalBsdf(bsdf,normal,wo,wi).value*parameters[4].xyz*cosine*diffuseFraction(bsdf,normal,wo,wi);
#endif
            value+=evalBsdf(bsdf,normal,wo,wi).value*parameters[4].xyz*cosine;
        }
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
float4 previousClip(float3 p) {
    float4 v=float4(p,1);
    return float4(dot(parameters[35],v),dot(parameters[36],v),dot(parameters[37],v),dot(parameters[38],v));
}
bool temporalReceiver(uint pixel,out uint previousPixel) {
    previousPixel=0; float4 hit=restirSurfaces[pixel*6];
    if(hit.w==0 || asuint(parameters[33]).z==0) return false;
    float3 position=restirSurfaces[pixel*6+2].xyz;
    float4 clip=previousClip(position);
    if(!all(isfinite(clip)) || clip.w<=0 || clip.z<0 || clip.z>clip.w) return false;
    float2 uv=clip.xy/clip.w*.5+.5;
    if(any(uv<0) || any(uv>=1)) return false;
    uint2 size=asuint(parameters[39]).xy,xy=uint2(uv*float2(size));
    previousPixel=xy.y*size.x+xy.x;
    if(previousSurfaces[previousPixel*6].w==0) return false;
    if(!paperRestir() && (any(asuint(previousSurfaces[previousPixel*6+4])!=asuint(restirSurfaces[pixel*6+4])) ||
        any(asint(previousSurfaces[previousPixel*6+5])!=asint(restirSurfaces[pixel*6+5])))) return false;
    float3 oldPosition=previousSurfaces[previousPixel*6+2].xyz;
    float depth=restirDepth(position);
    float difference=paperRestir() ? abs(restirDepth(oldPosition)-depth)/max(depth,1e-6) : distance(oldPosition,position);
    if(difference>parameters[34].x ||
        dot(previousSurfaces[previousPixel*6+1].xyz,restirSurfaces[pixel*6+1].xyz)<parameters[34].y) return false;
    if(asuint(previousSurfaces[previousPixel*6+3].w)!=asuint(parameters[34]).w) { restirError(); return false; }
    return true;
}
PathBsdf restirMaterial(float4 hit,uint4 key) {
    PathBsdf result; result.color=parameters[19].xyz; result.metallic=0; result.roughness=1; result.ggx=false;
#if !DK_PATH_EMPTY
    if(key.x==1) result=materialAt(key.y,hit.yz);
#if DK_PATH_VOXELS
    if(key.x==2) { PathPayload p={}; p.instance=key.y; p.value=key.w; result=voxelMaterial(p); }
#endif
#endif
    return result;
}
PathPayload restirPayload(uint pixel,bool previous=false) {
    float4 record=previous ? previousSurfaces[pixel*6] : restirSurfaces[pixel*6];
    uint4 key=asuint(previous ? previousSurfaces[pixel*6+4] : restirSurfaces[pixel*6+4]);
    PathPayload hit={}; hit.distance=record.x; hit.barycentric=record.yz; hit.found=(key.x==1 || key.x==2) ? 1u : 0u; hit.index=key.y;
#if DK_PATH_VOXELS
    if(key.x==2) {
        hit.kind=1; hit.instance=key.y; hit.face=key.z; hit.value=key.w;
        hit.cell=asint(previous ? previousSurfaces[pixel*6+5] : restirSurfaces[pixel*6+5]).xyz;
        hit.position=previous ? previousSurfaces[pixel*6+2].xyz : restirSurfaces[pixel*6+2].xyz;
    }
#endif
    return hit;
}
float temporalTarget(RestirSample s,uint pixel,bool previous=false,bool visibility=false,uint diagnosticPixel=0) {
    float4 hit=previous ? previousSurfaces[pixel*6] : restirSurfaces[pixel*6];
    float3 position=previous ? previousSurfaces[pixel*6+2].xyz : restirSurfaces[pixel*6+2].xyz;
    float3 normal=previous ? previousSurfaces[pixel*6+1].xyz : restirSurfaces[pixel*6+1].xyz;
    float3 wo=previous ? previousSurfaces[pixel*6+3].xyz : restirSurfaces[pixel*6+3].xyz;
    uint4 key=asuint(previous ? previousSurfaces[pixel*6+4] : restirSurfaces[pixel*6+4]);
    if(s.light==0xffffffffu || hit.w==0) return 0;
    float3 wi,point; float pdf;
    float3 value=restirEvaluate(s.light,s.uv,position,normal,restirMaterial(hit,key),wo,wi,point,pdf);
    float result=dot(value,float3(.2126,.7152,.0722));
    if(!isfinite(result) || result<0) restirError();
    if(visibility && result>0 && !restirVisible(s,restirPayload(pixel,previous),position,wi,point,diagnosticPixel)) return 0;
    return result;
}
RTXDI_DIReservoir sdkReservoir(RestirSample s,float M) {
    RTXDI_DIReservoir r=RTXDI_EmptyDIReservoir();
    r.M=M; r.weightSum=s.W; r.targetPdf=s.target;
    if(s.light!=0xffffffffu) r.lightData=s.light|RTXDI_DIReservoir_LightValidBit;
    return r;
}
void temporalRestir(uint pixel,uint epoch) {
    RestirSample fresh=loadRestir(pixel,epoch),result=fresh;
    uint oldPixel;
    if(temporalReceiver(pixel,oldPixel)) {
        float4 a=previousReservoirs[oldPixel*2],b=previousReservoirs[oldPixel*2+1];
        if(asuint(b.z)!=asuint(parameters[34]).w) restirError();
        if(!paperRestir() && asuint(b.w)>=asuint(parameters[33]).w) restirStat(pixel,4);
        else {
            RestirSample old; old.light=asuint(a.x); old.uv=a.yz; old.W=a.w; old.target=b.x; old.M=min(b.y,float(asuint(parameters[34]).z)); old.age=asuint(b.w);
            if(paperRestir()) old.M=min(old.M,20*fresh.M);
            RTXDI_DIReservoir combined=RTXDI_EmptyDIReservoir();
            RTXDI_CombineDIReservoirs(combined,sdkReservoir(fresh,fresh.M),0,fresh.target);
            float target=temporalTarget(old,pixel);
            if(RTXDI_CombineDIReservoirs(combined,sdkReservoir(old,old.M),restirRandom(pixel,epoch,0,5),target)) result=old;
            bool destinationVisible=!paperRestir() || temporalTarget(result,pixel,false,true,pixel)>0;
            float support=destinationVisible && temporalTarget(result,oldPixel,true,paperRestir(),pixel)>0 ? old.M : 0;
            RTXDI_FinalizeResampling(combined,1,fresh.M+support);
            result.W=combined.weightSum; result.target=combined.targetPdf; result.M=combined.M;
            result.age=min(old.age,0xfffffffeu)+1; // Ancestry, diagnostic only in v2; capped effective M controls history.
            if(combined.weightSum==0) result.light=0xffffffffu;
            if(!destinationVisible) result=zeroRestir(result);
            restirStat(pixel,2); if(support==0 && old.M>0) restirStat(pixel,7);
        }
    } else restirStat(pixel,3);
    if(!isfinite(result.W) || result.W<0 || !isfinite(result.M) || (result.target>0 && result.W==0)) restirError();
    restirMax(pixel,5,result.age); restirMax(pixel,6,uint(result.M));
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
    float4 hit=restirSurfaces[pixel*6];
    if(hit.w!=0) {
        float3 position=restirSurfaces[pixel*6+2].xyz,normal=restirSurfaces[pixel*6+1].xyz;
        uint4 config=asuint(parameters[40]); uint2 size=asuint(parameters[39]).xy;
        uint pixels[9]; float counts[9]; pixels[0]=pixel; counts[0]=own.M; uint count=1,age=own.age;
        RTXDI_DIReservoir combined=RTXDI_EmptyDIReservoir();
        RTXDI_CombineDIReservoirs(combined,sdkReservoir(own,own.M),0,own.target);
        uint side=2*config.y+1,center=(side*side)/2;
        for(uint i=0;i<config.x;++i) {
            uint offset=min(uint(restirRandom(pixel,epoch,i,6)*float(side*side-1)),side*side-2);
            if(offset>=center) ++offset;
            int2 xy=int2(pixel%size.x,pixel/size.x)+int2(offset%side,offset/side)-int(config.y);
            if(paperRestir()) {
                float angle=2*pi*restirRandom(pixel,epoch,0,6)+float(i)*2.39996323;
                float radius=float(config.y)*sqrt((float(i)+restirRandom(pixel,epoch,i,8))/float(config.x));
                xy=int2(pixel%size.x,pixel/size.x)+int2(round(radius*float2(cos(angle),sin(angle))));
            }
            bool accepted=all(xy>=0) && all(xy<int2(size));
            uint source=accepted ? uint(xy.y)*size.x+uint(xy.x) : 0;
            if(accepted) {
                for(uint j=0;j<count;++j) if(pixels[j]==source) accepted=false;
                float4 sourceHit=restirSurfaces[source*6];
                float depth=restirDepth(position);
                float difference=paperRestir() ? abs(restirDepth(restirSurfaces[source*6+2].xyz)-depth)/max(depth,1e-6) : distance(position,restirSurfaces[source*6+2].xyz);
                if(sourceHit.w==0 || difference>parameters[41].x ||
                    dot(normal,restirSurfaces[source*6+1].xyz)<parameters[41].y) accepted=false;
            }
            if(!accepted) { restirStat(pixel,9); continue; }
            if(asuint(restirSurfaces[source*6+3].w)!=epoch) restirError();
            RestirSample s=loadSpatial(source,epoch);
            pixels[count]=source; counts[count]=s.M; ++count; age=max(age,s.age);
            float target=temporalTarget(s,pixel);
            if(RTXDI_CombineDIReservoirs(combined,sdkReservoir(s,s.M),restirRandom(pixel,epoch,i,7),target)) result=s;
            restirStat(pixel,8);
        }
        float support=0;
        bool destinationVisible=!paperRestir() || temporalTarget(result,pixel,false,true,pixel)>0;
        for(uint j=0;j<count;++j) {
            uint source=pixels[j];
            if(destinationVisible && (paperRestir() && source==pixel ? true : temporalTarget(result,source,false,paperRestir(),pixel)>0)) support+=counts[j];
        }
        RTXDI_FinalizeResampling(combined,1,support);
        result.W=combined.weightSum; result.target=combined.targetPdf; result.M=combined.M; result.age=age;
        if(result.W==0) result.light=0xffffffffu;
        if(!destinationVisible) result=zeroRestir(result);
        if(support<combined.M && result.W>0) restirStat(pixel,10);
    }
    if(!isfinite(result.W) || result.W<0 || !isfinite(result.M) || (result.target>0 && result.W==0)) restirError();
    restirMax(pixel,11,uint(result.M)); restirMax(pixel,12,result.age);
    storeRestir(pixel,result,epoch);
}
