// Reduced transport, completed-history reprojection, reconstruction and optional compact fallback.
struct Parameters {
    float4 geometry,rayleigh,mie,extinction,absorption,ground,sun,forward,right,up,sunDirection;
    uint4 frame; float4 depth,origin;
};
[[vk::binding(0,0)]] ConstantBuffer<Parameters> p;
#include "RasterCamera.slang"
#include "../../engine/render/clouds/shaders/CloudTypes.slang"
struct CloudFrameParameters { CloudParameters model; CloudRayData origin; float4 forward,right,up,clear; };
[[vk::binding(10,0)]] ConstantBuffer<CloudFrameParameters> cloudFrame;
#define cp cloudFrame.model
#include "CloudCamera.slang"
struct ReconstructionParameters {
    uint4 size; // integration width/height, valid completed history, empty-space skipping
    float4 controls; // termination T, history weight, resolution divisor, optimization flags (smooth=1, sparse sun=2, sun counters=4)
    float4 previousClip[4]; // Current anchor-relative point, wind compensated, to previous clip.
    float4 cameraDelta; // Current minus previous anchor, without wind compensation.
    float4 previousForward; // Previous world forward and near distance.
};
[[vk::binding(16,0)]] ConstantBuffer<ReconstructionParameters> reconstruction;
[[vk::binding(15,0)]] RWStructuredBuffer<uint> statistics;
[[vk::binding(1,0)]] [[vk::image_format("rgba32f")]] RWTexture2D<float4> outputL;
[[vk::binding(6,0)]] [[vk::image_format("rgba32f")]] RWTexture2D<float4> outputT;
[[vk::binding(7,0)]] [[vk::image_format("rgba32f")]] RWTexture2D<float4> outputCloud;
#if DK_CLOUD_STAGE != 0
[[vk::binding(17,0)]] Texture2D<float4> currentL;
[[vk::binding(18,0)]] Texture2D<float4> currentT;
[[vk::binding(19,0)]] Texture2D<float4> currentCloud;
#endif
#if DK_CLOUD_STAGE == 1
[[vk::binding(20,0)]] Texture2D<float4> previousL;
[[vk::binding(21,0)]] Texture2D<float4> previousT;
[[vk::binding(22,0)]] Texture2D<float4> previousCloud;
#else
[[vk::binding(2,0)]] SamplerState linearClamp;
[[vk::binding(3,0)]] Texture2D<float4> transmittanceLut;
[[vk::binding(4,0)]] Texture2D<float4> multipleLut;
[[vk::binding(8,0)]] Texture2D<float> sceneDepth;
[[vk::binding(11,0)]] Texture2D<float2> weatherField;
[[vk::binding(12,0)]] Texture3D<float> baseField;
[[vk::binding(13,0)]] Texture3D<float> detailField;
[[vk::binding(14,0)]] SamplerState linearRepeat;
#define DK_STAGE 3
#include "../../engine/render/atmosphere/shaders/AtmosphereTransport.slang"
#include "../../engine/render/clouds/shaders/CloudTransport.slang"
#endif
#if DK_CLOUD_STAGE >= 2 && DK_CLOUD_COMPACT
[[vk::binding(23,0)]] RWStructuredBuffer<uint> fallbackPixels; // one fallback bit per full-resolution pixel
#endif
#if DK_CLOUD_STAGE >= 2
[[vk::binding(9,0)]] Texture2D<float4> surfaceColor;
#endif
float3 worldRay(float2 pixel) {
    float2 screen=(pixel+.5)/float2(p.frame.xy)*2-1;
    return cloudCameraDirection(screen);
}
bool matchingDepth(float a,float b) {
    return (a<0 && b<0) || (a>=0 && b>=0 && abs(a-b)<=max(2,.02*max(a,b)));
}
#if DK_CLOUD_STAGE != 1
float3 localRay(float2 pixel) {
    float2 screen=(pixel+.5)/float2(p.frame.xy)*2-1;
    return cameraRay(screen);
}
float geometryDistance(uint2 pixel) {
    float depth=sceneDepth.Load(int3(pixel,0));
    return depth<1 ? cameraDistance(depth,localRay(pixel))*1000 : -1;
}
float cellExit(float3 lattice,float3 velocity) {
    float3 fraction=frac(lattice),extent=select(velocity>=0,1-fraction,fraction);
    float3 distance=select(abs(velocity)>1e-12,extent/max(abs(velocity),1e-12),float3(1e20));
    return min(distance.x,min(distance.y,distance.z));
}
// Convex interpolation cannot exceed the corner maximum; erosion/profile cannot increase this bound.
float emptyDistance(CloudRayData ray,float t) {
    if(cp.flags.w!=0 || reconstruction.size.w==0) return 0;
    uint w=uint(cp.air.y),h=w,bx=uint(cp.air.z),by=bx,bz=bx;
    float3 position=ray.direction.xyz*t;
    float2 weather=(ray.weather.xy+position.xz/cp.scales.x)*float2(w,h)-.5;
    int2 wi=int2(floor(weather)); float weatherMax=0;
    for(int y=0;y<2;++y) for(int x=0;x<2;++x)
        weatherMax=max(weatherMax,weatherField.Load(int3((wi+int2(x,y))&int2(w-1,h-1),0)).x);
    float limit=cellExit(float3(weather,0),float3(ray.direction.xz/cp.scales.x*float2(w,h),0));
    if(weatherMax<=1-cp.cloud.x) return limit;
    float coverage=saturate((weatherMax-(1-cp.cloud.x))/cp.cloud.x);
    float3 base=(ray.base.xyz+position/cp.scales.y)*float3(bx,by,bz)-.5;
    int3 bi=int3(floor(base)); float baseMax=0;
    for(int z=0;z<2;++z) for(int y=0;y<2;++y) for(int x=0;x<2;++x)
        baseMax=max(baseMax,baseField.Load(int4((bi+int3(x,y,z))&int3(bx-1,by-1,bz-1),0)));
    return baseMax<=1-coverage ? min(limit,cellExit(base,ray.direction.xyz/cp.scales.y*float3(bx,by,bz))) : 0;
}
// Keep the reference quadrature exactly; only omit texture reads whose contribution is zero.
float sunlightDensity(CloudRayData ray,float3 displacement,float height,inout uint4 reads) {
    ++reads.x;
    if(cp.flags.w!=0) return cp.cloud.y*cp.cloud.w;
    if(height<=cp.geometry.z || height>=cp.geometry.w) return 0;
    ++reads.y;
    float2 weather=weatherField.SampleLevel(linearRepeat,ray.weather.xy+displacement.xz/cp.scales.x,0);
    float coverage=saturate((weather.x-(1-cp.cloud.x))/cp.cloud.x);
    float h=(height-cp.geometry.z)/(cp.geometry.w-cp.geometry.z);
    float profile=cloudSmooth01(h/cp.scales.w)*cloudSmooth01((.6+.4*weather.y-h)/cp.lighting.w);
    bool sparse=(uint(reconstruction.controls.w)&2u)!=0;
    // Base and detail fields lie in [0,1]; profile cannot increase density.
    if(sparse && profile<=1-coverage) return 0;
    ++reads.z;
    float base=baseField.SampleLevel(linearRepeat,ray.base.xyz+displacement/cp.scales.y,0);
    float shaped=(base*profile-(1-coverage))/.35;
    if(sparse && shaped<=0) return 0;
    ++reads.w;
    float detail=detailField.SampleLevel(linearRepeat,ray.detail.xyz+displacement/cp.scales.z,0);
    return cp.cloud.y*saturate(shaped-.2*detail);
}
float sunlightOptical(CloudRayData ray,float3 position,float h,float mu,inout uint4 reads) {
    uint steps=max(cp.counts.y,abs(mu)<.2 ? 32u : 0u);
    float4 segments; bool ground; uint count=cloudShell(h,mu,segments,ground);
    if(ground) return -1;
    if(cp.flags.z==0) return 0;
    float optical=0;
    [loop] for(uint k=0;k<count;++k) {
        float a=segments[2*k],b=segments[2*k+1],ds=(b-a)/float(steps);
        [loop] for(uint j=0;j<steps;++j) {
            float t=a+(float(j)+.5)*ds;
            optical+=sunlightDensity(ray,position+cp.sun.xyz*t,cloudHeightAt(h,mu,t),reads)*cp.cloud.z*ds;
        }
    }
    return optical;
}
void addCount64(uint index,uint count) {
    uint prior; InterlockedAdd(statistics[index],count,prior);
    if(prior>0xffffffffu-count) InterlockedAdd(statistics[index+1],1);
}
struct Medium { float4 L,T,cloud; };
Medium trace(uint2 pixel) {
    float2 screen=(float2(pixel)+.5)/float2(p.frame.xy)*2-1;
    CameraMediumRay camera=mediumRay(screen); float3 direction=camera.direction;
    CloudRayData ray=cloudCameraRay(screen,camera);
    float mu=camera.mu,cosine=camera.cosine,geometry=geometryDistance(pixel);
    if(camera.height<0) { Medium value; value.L=float4(0,0,0,geometry); value.T=0; value.cloud=float4(0,0,0,1); return value; }
    float3 segment=path(camera.height,mu);
    float distance=geometry>=0 ? geometry : max(0,segment.y*1000);
    float begin=max(0,segment.x*1000),end=min(distance,segment.y*1000);
    float4 shell; bool ground; uint shellCount=cloudShell(ray.geometry.x,mu,shell,ground);
    float cuts[6]; uint count=0; cuts[count++]=begin;
    for(uint k=0;k<shellCount;++k) { cuts[count++]=clamp(shell[2*k],begin,max(begin,end)); cuts[count++]=clamp(shell[2*k+1],begin,max(begin,end)); }
    cuts[count++]=max(begin,end);
    float3 L=0,T=1,cloudL=0; float cloudT=1,opacity=0,moment=0;
    float3 phases=float3(cloudHG(cosine,cp.lighting.x),cloudHG(cosine,cp.lighting.x*.5),cloudHG(cosine,cp.lighting.x*.25));
    float3 sunDelta=direction-p.sunDirection.xyz;
    bool solar=p.frame.w!=0 && dot(sunDelta,sunDelta)<=p.sunDirection.w;
    uint4 sunReads=0;
    bool stopped=false; uint visited=0,lighting=0,empty=0,terminated=0,densitySamples=0;
    [loop] for(uint k=1;k<count;++k) {
        float first=cuts[k-1],last=cuts[k]; if(last<=first) continue;
        bool cloudy=false; float middle=(first+last)*.5;
        for(uint j=0;j<shellCount;++j) cloudy=cloudy || (middle>=shell[2*j] && middle<shell[2*j+1]);
        uint steps=cloudy ? max(cp.counts.x,uint(min(256.,ceil((last-first)/200)))) : cp.counts.x;
        if(stopped) { terminated+=steps; continue; }
        [loop] for(uint i=0;i<steps;) {
            uint next=i+1;
            float a=lerp(first,last,cloudy ? float(i)/steps : cloudNode(i,steps));
            bool skip=false;
            if(cloudy) {
                float step=(last-first)/steps;
                float limit=emptyDistance(ray,a);
                uint jump=uint(min(float(steps-i),max(0,floor(max(0,limit-.001)/step))));
                if(jump>0) { next=i+jump; skip=true; empty+=jump; }
            }
            float b=lerp(first,last,cloudy ? float(next)/steps : cloudNode(next,steps));
            float ds=b-a,t=(a+b)*.5,h=cloudHeightAt(ray.geometry.x,mu,t);
            float sunMu=clamp(((cp.geometry.x+ray.geometry.x)*ray.geometry.z+t*cosine)/(cp.geometry.x+h),-1,1);
            float3 sunAir=cp.flags.y!=0 ? sunlight(h*.001,sunMu) : float3(1);
            float3 extinction=0,airSource=0,cloudSource=0;
            if(cp.flags.y!=0) {
                float3 rayleigh,mie; medium(h*.001,rayleigh,mie,extinction);
                float rayPhase=3*(1+cosine*cosine)/(16*PI),miePhase=cloudHG(cosine,p.extinction.w);
                airSource=(rayleigh*rayPhase+mie*miePhase)*sunAir;
                if(p.up.w>0) airSource+=(rayleigh+mie)*multiple(h*.001,sunMu);
                airSource*=p.sun.rgb*.001; extinction*=.001;
            }
            float sigma=0;
            if(cloudy && !skip) {
                uint samples=ds>100 ? 4 : 1;
                for(uint sample=0;sample<samples;++sample) {
                    float st=a+(float(sample)+.5)*ds/samples;
                    sigma+=cloudDensity(ray,ray.direction.xyz*st,cloudHeightAt(ray.geometry.x,mu,st));
                }
                sigma*=cp.cloud.z/samples; densitySamples+=samples;
            }
            if(sigma>0) {
                ++lighting; float tau=sunlightOptical(ray,ray.direction.xyz*t,h,sunMu,sunReads);
                float3 source=cp.sky.xyz;
                if(tau>=0) {
                    float phase=(1-cp.lighting.z)*phases.x*exp(-tau)+cp.lighting.z*(2.0/3*phases.y*exp(-tau*.5)+1.0/3*phases.z*exp(-tau*.25));
                    source+=cp.irradiance.xyz*sunAir*phase;
                }
                cloudSource=sigma*cp.lighting.y*source;
            }
            extinction+=sigma; float3 weightT=T*weight(extinction,ds);
            L+=weightT*(airSource+cloudSource); cloudL+=weightT*cloudSource; T*=exp(-extinction*ds);
            float cw=cloudT*cloudAbsorbed(sigma*ds); cloudT*=exp(-sigma*ds); opacity+=cw; moment+=cw*t;
            ++visited; i=next;
            if(!solar && reconstruction.controls.x>0 && max(cloudT,max(T.x,max(T.y,T.z)))<=reconstruction.controls.x) { stopped=true; terminated+=steps-i; break; }
        }
    }
    InterlockedAdd(statistics[0],visited); InterlockedAdd(statistics[1],lighting);
    InterlockedAdd(statistics[2],empty); InterlockedAdd(statistics[3],terminated);
    addCount64(8,densitySamples);
    if((uint(reconstruction.controls.w)&4u)!=0) {
        addCount64(10,sunReads.x); addCount64(12,sunReads.y); addCount64(14,sunReads.z); addCount64(16,sunReads.w);
    }
    Medium value; value.L=float4(L,geometry); value.T=float4(T,opacity>0 ? moment/opacity : 0); value.cloud=float4(cloudL,cloudT); return value;
}
#endif
#if DK_CLOUD_STAGE >= 2
// A large first difference can be a smooth ramp. Fit a local plane and require
// small residual in BOTH transmission and HDR radiance before bypassing the range guard.
bool smoothFootprint(int2 base,float geometry) {
    float4 transmission[9]; float3 radiance[9]; float scale=0;
    for(int y=-1;y<=1;++y) for(int x=-1;x<=1;++x) {
        int2 point=clamp(base+int2(x,y),0,int2(reconstruction.size.xy)-1);
        float4 a=currentL.Load(int3(point,0)),b=currentT.Load(int3(point,0)),c=currentCloud.Load(int3(point,0));
        if(!matchingDepth(geometry,a.a)) return false;
        uint i=(y+1)*3+x+1; transmission[i]=float4(b.rgb,c.a); radiance[i]=a.rgb;
        scale=max(scale,max(a.x,max(a.y,a.z)));
    }
    float4 tx=(transmission[5]-transmission[3])*.5,ty=(transmission[7]-transmission[1])*.5;
    float3 lx=(radiance[5]-radiance[3])*.5,ly=(radiance[7]-radiance[1])*.5;
    float errorT=0,errorL=0;
    for(int y=-1;y<=1;++y) for(int x=-1;x<=1;++x) {
        uint i=(y+1)*3+x+1;
        float4 t=abs(transmission[i]-(transmission[4]+x*tx+y*ty));
        float3 l=abs(radiance[i]-(radiance[4]+x*lx+y*ly));
        errorT=max(errorT,max(max(t.x,t.y),max(t.z,t.w))); errorL=max(errorL,max(l.x,max(l.y,l.z)));
    }
    // The quarter-resolution footprint spans twice the angle: use a tighter residual guard.
    float tolerance=reconstruction.controls.z>2 ? .001 : .002;
    return errorT<=tolerance && errorL<=tolerance*max(.001,scale);
}
void compose(uint2 pixel,float3 L,float3 T,float4 C,float representative,float geometry,CameraMediumRay camera) {
    float3 direction=camera.direction;
    float3 segment=path(camera.height,camera.mu),terminal=0;
    float distance=geometry>=0 ? geometry : max(0,segment.y*1000);
    if(!(segment.z>0 && distance>=segment.y*1000)) {
        if(geometry>=0) terminal=surfaceColor.Load(int3(pixel,0)).rgb;
        else {
            if(cp.flags.y==0) terminal=cloudFrame.clear.rgb;
            float3 delta=direction-p.sunDirection.xyz;
            if(p.frame.w!=0 && dot(delta,delta)<=p.sunDirection.w) terminal+=p.sun.rgb*p.sun.w;
        }
    }
    outputL[pixel]=float4(L+T*terminal,1); outputT[pixel]=C; outputCloud[pixel]=float4(T,representative);
}
#endif
#if DK_CLOUD_STAGE == 3
groupshared uint tileCount;
groupshared uint2 tilePixels[64];
#endif
[numthreads(8,8,1)]
void computeMain(uint3 id:SV_DispatchThreadID,uint3 local:SV_GroupThreadID) {
#if DK_CLOUD_STAGE == 3
    uint lane=local.y*8+local.x;
    if(lane==0) tileCount=0;
    GroupMemoryBarrierWithGroupSync();
    if(all(id.xy<p.frame.xy)) {
        if(fallbackPixels[id.y*p.frame.x+id.x]!=0) {
            uint slot; InterlockedAdd(tileCount,1,slot); tilePixels[slot]=id.xy;
        }
    }
    GroupMemoryBarrierWithGroupSync();
    if(lane>=tileCount) return;
    uint2 pixel=tilePixels[lane];
    Medium value=trace(pixel);
    CameraMediumRay camera=mediumRay((float2(pixel)+.5)/float2(p.frame.xy)*2-1);
    compose(pixel,value.L.rgb,value.T.rgb,value.cloud,value.T.a,value.L.a,camera);
#elif DK_CLOUD_STAGE == 0
    if(any(id.xy>=reconstruction.size.xy)) return;
    Medium value=trace(min(id.xy*uint(reconstruction.controls.z),p.frame.xy-1)); outputL[id.xy]=value.L; outputT[id.xy]=value.T; outputCloud[id.xy]=value.cloud;
#elif DK_CLOUD_STAGE == 1
    if(any(id.xy>=reconstruction.size.xy)) return;
    float4 L=currentL.Load(int3(id.xy,0)),T=currentT.Load(int3(id.xy,0)),C=currentCloud.Load(int3(id.xy,0));
    float3 lowL=L.rgb,highL=L.rgb,lowT=T.rgb,highT=T.rgb; float4 lowC=C,highC=C;
    bool edge=false;
    for(int y=-1;y<=1;++y) for(int x=-1;x<=1;++x) {
        int2 sample=clamp(int2(id.xy)+int2(x,y),0,int2(reconstruction.size.xy)-1);
        float4 nL=currentL.Load(int3(sample,0)),nT=currentT.Load(int3(sample,0)),nC=currentCloud.Load(int3(sample,0));
        edge=edge || !matchingDepth(L.a,nL.a);
        lowL=min(lowL,nL.rgb); highL=max(highL,nL.rgb); lowT=min(lowT,nT.rgb); highT=max(highT,nT.rgb); lowC=min(lowC,nC); highC=max(highC,nC);
    }
    float2 pixel=min(id.xy*uint(reconstruction.controls.z),p.frame.xy-1);
    float3 origin=cloudCameraOrigin((pixel+.5)/float2(p.frame.xy)*2-1);
    float3 direction=worldRay(pixel); float4 position=float4(origin+direction*T.a,1);
    float4 clip=float4(dot(reconstruction.previousClip[0],position),dot(reconstruction.previousClip[1],position),
        dot(reconstruction.previousClip[2],position),dot(reconstruction.previousClip[3],position));
    float2 oldPixel=(clip.xy/max(clip.w,1e-8)*.5+.5)*float2(p.frame.xy)-.5;
    float2 old=oldPixel/reconstruction.controls.z; int2 base=int2(floor(old)); float2 fraction=frac(old);
    bool accepted=reconstruction.size.z!=0 && !edge && T.a>0 && C.a<.999 && clip.w>0 &&
        (p.origin.w==0 || clip.z>=0) && all(old>=0) && all(old<=float2(reconstruction.size.xy)-1);
    float4 priorL=0,priorT=0,priorC=0;
    if(accepted) {
        float3 oldPoint=reconstruction.cameraDelta.xyz+origin+direction*L.a;
        float predicted=L.a<0 ? -1 : (p.origin.w!=0 ? dot(oldPoint,reconstruction.previousForward.xyz)-reconstruction.previousForward.w : length(oldPoint));
        // Negative orthographic distance is behind the old near plane, not sky.
        accepted=accepted && (L.a<0 || predicted>=0);
        for(int y=0;y<2;++y) for(int x=0;x<2;++x) {
            int2 point=min(base+int2(x,y),int2(reconstruction.size.xy)-1);
            float4 a=previousL.Load(int3(point,0)),b=previousT.Load(int3(point,0)),c=previousCloud.Load(int3(point,0));
            float w=(x ? fraction.x : 1-fraction.x)*(y ? fraction.y : 1-fraction.y);
            if(w>1e-5) accepted=accepted && matchingDepth(predicted,a.a) && abs(c.a-C.a)<.01;
            priorL+=w*a; priorT+=w*b; priorC+=w*c;
        }
        accepted=accepted && abs(priorT.a-T.a)<=max(100,T.a*.25);
    }
    if(accepted) {
        L.rgb=lerp(L.rgb,clamp(priorL.rgb,lowL,highL),reconstruction.controls.y);
        T.rgb=lerp(T.rgb,clamp(priorT.rgb,lowT,highT),reconstruction.controls.y);
        C=lerp(C,clamp(priorC,lowC,highC),reconstruction.controls.y);
        InterlockedAdd(statistics[6],1);
    } else InterlockedAdd(statistics[7],1);
    outputL[id.xy]=L; outputT[id.xy]=T; outputCloud[id.xy]=C;
#else
    if(any(id.xy>=p.frame.xy)) return;
#if DK_CLOUD_COMPACT
    fallbackPixels[id.y*p.frame.x+id.x]=0;
#endif
    float geometry=geometryDistance(id.xy); float2 halfPixel=float2(id.xy)/reconstruction.controls.z;
    int2 base=int2(floor(halfPixel)); float2 fraction=frac(halfPixel);
    float3 L=0,T=0; float4 C=0; float representative=0,total=0,minC=1,maxC=0; float3 minT=1,maxT=0;
    for(int y=0;y<2;++y) for(int x=0;x<2;++x) {
        int2 point=min(base+int2(x,y),int2(reconstruction.size.xy)-1);
        float4 a=currentL.Load(int3(point,0)),b=currentT.Load(int3(point,0)),c=currentCloud.Load(int3(point,0));
        float w=(x ? fraction.x : 1-fraction.x)*(y ? fraction.y : 1-fraction.y);
        if(w>1e-5 && matchingDepth(geometry,a.a)) { L+=w*a.rgb; T+=w*b.rgb; C+=w*c; representative+=w*b.a; total+=w; minT=min(minT,b.rgb); maxT=max(maxT,b.rgb); minC=min(minC,c.a); maxC=max(maxC,c.a); }
    }
    // A two-tap pair can straddle an unresolved cloud edge with similar endpoint T.
    // Check the surrounding footprint before interpolating a non-anchor pixel.
    if(any((id.xy%uint(reconstruction.controls.z))!=0)) for(int y=-1;y<=1;++y) for(int x=-1;x<=1;++x) {
        int2 point=clamp(base+int2(x,y),0,int2(reconstruction.size.xy)-1);
        float4 a=currentL.Load(int3(point,0)),b=currentT.Load(int3(point,0)),c=currentCloud.Load(int3(point,0));
        if(!matchingDepth(geometry,a.a)) total=0;
        minT=min(minT,b.rgb); maxT=max(maxT,b.rgb); minC=min(minC,c.a); maxC=max(maxC,c.a);
    }
    CameraMediumRay camera=mediumRay((float2(id.xy)+.5)/float2(p.frame.xy)*2-1);
    if(camera.height<0) { outputL[id.xy]=float4(0,0,0,1); outputT[id.xy]=float4(0,0,0,1); outputCloud[id.xy]=0; return; }
    float3 direction=camera.direction,sunDelta=direction-p.sunDirection.xyz;
    bool solar=p.frame.w!=0 && dot(sunDelta,sunDelta)<=p.sunDirection.w;
    bool rangeEdge=max(maxC-minC,max(maxT.x-minT.x,max(maxT.y-minT.y,maxT.z-minT.z)))>.02;
    if(rangeEdge && total>=.999 && !solar && (uint(reconstruction.controls.w)&1u)!=0)
        rangeEdge=!smoothFootprint(base,geometry);
    if(total<.999 || solar || rangeEdge) {
#if DK_CLOUD_COMPACT
        fallbackPixels[id.y*p.frame.x+id.x]=1;
        // Define every output texel before the queued partial writer preserves the image.
        outputL[id.xy]=0; outputT[id.xy]=0; outputCloud[id.xy]=0;
        InterlockedAdd(statistics[5],1); return;
#else
        Medium value=trace(id.xy); L=value.L.rgb; T=value.T.rgb; C=value.cloud; representative=value.T.a;
        InterlockedAdd(statistics[5],1);
#endif
    } else { L/=total; T/=total; C/=total; representative/=total; InterlockedAdd(statistics[4],1); }
    compose(id.xy,L,T,C,representative,geometry,camera);
#endif
}
