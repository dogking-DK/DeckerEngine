#ifndef DK_PATH_EMPTY
#define DK_PATH_EMPTY 0
#endif
struct PathPayload { float distance; float2 barycentric; uint index; uint found; };
#if !DK_PATH_EMPTY
[[vk::binding(0,0)]] RaytracingAccelerationStructure scene;
[[vk::binding(1,0)]] StructuredBuffer<float4> triangles;
#endif
[[vk::binding(2,0)]] StructuredBuffer<float4> parameters;
[[vk::binding(3,0)]] RWStructuredBuffer<float4> output;
[[vk::binding(4,0)]] StructuredBuffer<float4> inputRays;
#include "path-accumulate.slang"
#if !DK_PATH_EMPTY
#if DK_PATH_TEXTURES
[[vk::binding(5,0)]] Texture2D<float4> baseTextures[64];
[[vk::binding(6,0)]] SamplerState baseSamplers[64];
#endif
float4 baseColor(uint index,float2 barycentric) {
    float4 factor=triangles[index*9+4];
#if DK_PATH_TEXTURES
    float4 a=triangles[index*9+6],b=triangles[index*9+7]; uint texture=asuint(b.z);
    if(texture!=0xffffffffu) {
        if(texture>=64) return asfloat(0x7fc00000u).xxxx;
        float2 uv=a.xy*(1-barycentric.x-barycentric.y)+a.zw*barycentric.x+b.xy*barycentric.y;
        uint resource=NonUniformResourceIndex(texture);
        factor*=baseTextures[resource].SampleLevel(baseSamplers[resource],uv,0);
    }
#endif
    return factor;
}
#endif
static const float pi=3.14159265358979323846;
#include "path-bsdf.slang"
#if !DK_PATH_EMPTY
PathBsdf materialAt(uint index,float2 barycentric) {
    float4 material=triangles[index*9+8];
    PathBsdf result; result.color=baseColor(index,barycentric).xyz; result.metallic=material.x; result.roughness=material.y; result.ggx=material.z!=0; return result;
}
#endif
static const float infinity=3.402823466e+38;
float largest(float3 v) { return max(v.x,max(v.y,v.z)); }
uint mixBits(uint x) { x^=x>>16; x*=0x7feb352d; x^=x>>15; x*=0x846ca68b; return x^(x>>16); }
float randomValue(uint pixel,uint sample,uint bounce,uint dimension) {
    uint4 p=asuint(parameters[1]); uint h=mixBits(p.x^0xa511e9b3);
    h=mixBits(h^p.y); h=mixBits(h^pixel); h=mixBits(h^sample); h=mixBits(h^bounce); h=mixBits(h^dimension);
    return min((float(h>>8)+.5)/16777216.0,asfloat(0x3f7fffff));
}
float weight(float a,float b) { if(a<=0) return 0; if(b<=0) return 1; float r=b/a; return 1/(1+r*r); }
float nextFloat(float v,float direction) {
    if(v==0) return asfloat(direction>0 ? 1u : 0x80000001u);
    uint bits=asuint(v); return asfloat((v>0)==(direction>0) ? bits+1 : bits-1);
}
#if !DK_PATH_EMPTY
float3 offsetOrigin(uint index,float3 position,float3 direction) {
    float3 extent=max(max(abs(triangles[index*9].xyz),abs(triangles[index*9+1].xyz)),abs(triangles[index*9+2].xyz))+abs(position);
    float error=64*1.1920928955078125e-7*largest(extent);
    float3 n=normalize(triangles[index*9+3].xyz); if(dot(n,direction)<0) n=-n;
    float3 result=position+n*error;
    for(uint k=0;k<3;++k) if(n[k]!=0) result[k]=nextFloat(result[k],n[k]);
    return result;
}
float3 positionOf(PathPayload hit) {
    return triangles[hit.index*9].xyz*(1-hit.barycentric.x-hit.barycentric.y)+
        triangles[hit.index*9+1].xyz*hit.barycentric.x+triangles[hit.index*9+2].xyz*hit.barycentric.y;
}
float3 normalOf(PathPayload hit,float3 direction) {
    float3 normal=normalize(triangles[hit.index*9+3].xyz); return dot(normal,direction)<0 ? normal : -normal;
}
float areaPdf(uint index,float3 from,float3 point) {
    float4 a=triangles[index*9],b=triangles[index*9+1],n=triangles[index*9+3];
    if(b.w<=0) return 0;
    float3 delta=point-from; float distance2=dot(delta,delta); if(distance2<=0) return 0;
    float cosine=-dot(normalize(n.xyz),delta/sqrt(distance2)); if(n.w!=0) cosine=abs(cosine);
    if(cosine<=0) return 0;
    return parameters[4].w*(b.w/parameters[3].w)*distance2/(a.w*cosine);
}
bool visible(uint index,float3 position,float3 direction,uint lightIndex,float3 point,inout uint rays) {
    RayDesc ray; ray.Origin=offsetOrigin(index,position,direction); ray.Direction=direction; ray.TMin=0; ray.TMax=infinity;
    if(lightIndex!=0xffffffffu) {
        float3 end=offsetOrigin(lightIndex,point,-direction),delta=end-ray.Origin; float distance=length(delta);
        if(distance<=0 || dot(delta,direction)<=0) return false;
        ray.Direction=delta/distance; ray.TMax=nextFloat(distance,-1);
    }
    PathPayload payload={}; payload.found=1; ++rays;
    TraceRay(scene,RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH|RAY_FLAG_SKIP_CLOSEST_HIT_SHADER,255,1,2,1,ray,payload);
    return payload.found==0;
}
float3 directLight(PathPayload hit,float3 position,float3 normal,PathBsdf bsdf,float3 wo,uint pixel,uint sample,uint depth,inout uint rays) {
    uint4 flags=asuint(parameters[2]); bool bsdfOnly=flags.x!=0,solar=flags.y!=0;
    float classPdf=parameters[4].w;
    if((bsdfOnly && !solar) || (!bsdfOnly && classPdf==0)) return 0;
    int selected=bsdfOnly ? 0 : int(randomValue(pixel,sample,depth,2)/classPdf);
    float3 wi=0,radiance=0,point=0; float pdf=0; bool delta=false; uint light=0xffffffffu;
    if(!bsdfOnly && largest(parameters[3].xyz)>0 && selected--==0) {
        float z=1-2*randomValue(pixel,sample,depth,4),r=sqrt(max(0.0,1-z*z)),phi=2*pi*randomValue(pixel,sample,depth,5);
        wi=float3(r*cos(phi),r*sin(phi),z); radiance=parameters[3].xyz; pdf=classPdf/(4*pi);
    } else if(solar && (bsdfOnly || selected--==0)) {
        wi=normalize(parameters[5].xyz); radiance=parameters[4].xyz; pdf=bsdfOnly ? 1 : classPdf; delta=true;
    } else if(parameters[3].w>0) {
        float target=randomValue(pixel,sample,depth,3)*parameters[3].w; uint lo=0,hi=flags.z;
        while(lo<hi) { uint mid=(lo+hi)/2; if(target<triangles[mid*9+2].w) hi=mid; else lo=mid+1; }
        if(lo==flags.z) return 0; light=lo;
        float root=sqrt(randomValue(pixel,sample,depth,4)),v=randomValue(pixel,sample,depth,5);
        point=triangles[light*9].xyz*(1-root)+triangles[light*9+1].xyz*(root*(1-v))+triangles[light*9+2].xyz*(root*v);
        float3 displacement=point-position; if(dot(displacement,displacement)==0) return 0;
        wi=normalize(displacement); radiance=triangles[light*9+5].xyz; pdf=areaPdf(light,position,point);
    }
    float cosine=dot(normal,wi);
    if(cosine<=0 || pdf<=0 || !visible(hit.index,position,wi,light,point,rays)) return 0;
    BsdfEval evaluated=evalBsdf(bsdf,normal,wo,wi);
    return evaluated.value*radiance*(cosine*(delta ? 1 : weight(pdf,evaluated.pdf))/pdf);
}
#endif
#include "path-air.slang"
RayDesc primaryRay(uint2 coordinate,uint pixel,uint sample,out float3 originOffset) {
    RayDesc ray; originOffset=0;
    if(asuint(parameters[2]).w!=0) {
        uint stride=asuint(parameters[12]).x!=0 ? 6u : 2u;
        float4 a=inputRays[pixel*stride],b=inputRays[pixel*stride+1]; ray.Origin=a.xyz; ray.TMin=a.w; ray.Direction=normalize(b.xyz); ray.TMax=b.w;
    } else {
        uint counter=pixel+asuint(parameters[10]).x;
        float2 subpixel=float2(randomValue(counter,sample,0,0),randomValue(counter,sample,0,1));
        if(asuint(parameters[12]).x!=0) subpixel=float2(airRandom(counter,sample,0,1),airRandom(counter,sample,0,1,1));
        float3 near=parameters[7].xyz+parameters[8].xyz*(float(coordinate.x)+subpixel.x)+parameters[9].xyz*(float(coordinate.y)+subpixel.y);
        if(parameters[6].w!=0) {
            originOffset=near; ray.Origin=parameters[6].xyz+near; ray.Direction=parameters[31].xyz; ray.TMin=0; ray.TMax=parameters[31].w;
        } else {
            float scale=largest(abs(near)); float3 scaled=near/scale; float distance=length(scaled);
            ray.Origin=parameters[6].xyz; ray.TMin=distance*scale; ray.Direction=scaled/distance; ray.TMax=ray.TMin*parameters[5].w;
        }
    }
    return ray;
}
#if !DK_PATH_EMPTY
float3 tracePath(RayDesc ray,uint pixel,uint sample,inout uint rays,out float4 firstHit,out float4 firstNormal) {
    uint4 counts=asuint(parameters[1]),flags=asuint(parameters[2]);
    float3 radiance=0,throughput=1,previous=0; float bsdfPdf=0;
    firstHit=float4(-1,0,0,0); firstNormal=0;
    for(uint depth=0;depth<=counts.z;++depth) {
        PathPayload hit={}; ++rays; TraceRay(scene,RAY_FLAG_NONE,255,0,2,0,ray,hit);
        if(hit.found==0) {
            radiance+=throughput*parameters[3].xyz*(depth!=0 && flags.x==0 ? weight(bsdfPdf,parameters[4].w/(4*pi)) : 1); break;
        }
        float3 position=positionOf(hit),normal=normalOf(hit,ray.Direction);
        if(depth==0) { firstHit=float4(hit.distance,hit.barycentric,float(hit.index+1)); firstNormal=float4(normal,dot(ray.Direction,triangles[hit.index*9+3].xyz)<0 ? 1 : -1); }
        PathBsdf bsdf=materialAt(hit.index,hit.barycentric); float3 emission=triangles[hit.index*9+5].xyz;
        radiance+=throughput*emission*(depth!=0 && flags.x==0 ? weight(bsdfPdf,areaPdf(hit.index,previous,position)) : 1);
        if(depth==counts.z || (!bsdf.ggx && largest(bsdf.color)==0)) break;
        radiance+=throughput*directLight(hit,position,normal,bsdf,-ray.Direction,pixel,sample,depth,rays);
        float3 wi=sampleBsdf(bsdf,normal,-ray.Direction,randomValue(pixel,sample,depth,6),randomValue(pixel,sample,depth,7));
        BsdfEval evaluated=evalBsdf(bsdf,normal,-ray.Direction,wi); if(evaluated.pdf<=0) break;
        bsdfPdf=evaluated.pdf; previous=position; throughput*=evaluated.value*(max(0.0,dot(normal,wi))/bsdfPdf);
        if(largest(throughput)==0) break;
        if(counts.w!=0 && depth+1>=counts.w) {
            float survival=clamp(largest(throughput),.05,.95);
            if(randomValue(pixel,sample,depth,8)>=survival) break; throughput/=survival;
        }
        ray.Origin=offsetOrigin(hit.index,position,wi); ray.Direction=wi; ray.TMin=0; ray.TMax=infinity;
    }
    return radiance;
}
#endif
void runPathPixel(uint2 coordinate) {
    uint4 image=asuint(parameters[0]),tile=asuint(parameters[11]);
    uint localPixel=coordinate.y*image.x+coordinate.x;
    if(tile.w!=0) coordinate+=tile.xy;
    uint pixel=tile.w!=0 ? coordinate.y*tile.z+coordinate.x : localPixel;
    Accumulation a=beginAccumulation(pixel);
    for(uint sample=image.z;sample<image.z+image.w;++sample) {
        float4 hit=0,n=0; float3 value=0,originOffset; RayDesc ray=primaryRay(coordinate,pixel,sample,originOffset);
        if(asuint(parameters[12]).x!=0) {
            float4 initial=asuint(parameters[2]).w!=0 ? inputRays[pixel*6+2] : parameters[13];
            AirLocation location; location.radial=normalize(initial.xyz); location.height=initial.w;
            bool fixed=asuint(parameters[2]).w!=0;
            location.weather=(fixed ? inputRays[pixel*6+3] : parameters[26]).xy;
            location.base=(fixed ? inputRays[pixel*6+4] : parameters[27]).xyz;
            location.detail=(fixed ? inputRays[pixel*6+5] : parameters[28]).xyz;
            if(!fixed && parameters[6].w!=0) {
                location=airMove(location,originOffset);
                if(asuint(parameters[12]).y!=0 && location.height<0) airError(4);
            }
            value=traceAir(ray,location,pixel+asuint(parameters[10]).x,sample,a.rays,hit,n);
        }
#if !DK_PATH_EMPTY
        else value=tracePath(ray,pixel+asuint(parameters[10]).x,sample,a.rays,hit,n);
#endif
        addSample(a,value,sample,hit,n);
    }
    finishAccumulation(a,pixel,localPixel);
}
#if !DK_PATH_EMPTY
[shader("raygeneration")]
void pathMain() { runPathPixel(DispatchRaysIndex().xy); }
[shader("miss")]
void pathMiss(inout PathPayload payload) { payload.found=0; }
[shader("miss")]
void shadowMiss(inout PathPayload payload) { payload.found=0; }
[shader("closesthit")]
void pathHit(inout PathPayload payload,BuiltInTriangleIntersectionAttributes attributes) {
    payload.distance=RayTCurrent(); payload.barycentric=attributes.barycentrics; payload.index=PrimitiveIndex(); payload.found=1;
}
[shader("anyhit")]
void pathAnyHit(inout PathPayload payload,BuiltInTriangleIntersectionAttributes attributes) {
    uint index=PrimitiveIndex(); float4 n=triangles[index*9+3];
    if((n.w==0 && dot(n.xyz,WorldRayDirection())>=0) ||
       (triangles[index*9+7].w!=0 && baseColor(index,attributes.barycentrics).w<triangles[index*9+5].w)) IgnoreHit();
}
#else
[shader("compute")]
[numthreads(64,1,1)]
void airEmptyMain(uint3 id:SV_DispatchThreadID) {
    uint4 image=asuint(parameters[0]); if(id.x<image.x*image.y) runPathPixel(uint2(id.x%image.x,id.x/image.x));
}
#endif
