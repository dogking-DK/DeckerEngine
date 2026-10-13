// Three graph stages: half-size transport, completed-history reprojection, depth-aware reconstruction.
struct Parameters {
    float4 geometry,rayleigh,mie,extinction,absorption,ground,sun,forward,right,up,sunDirection;
    uint4 frame; float4 depth;
};
[[vk::binding(0,0)]] ConstantBuffer<Parameters> p;
#include "../../engine/render/clouds/shaders/CloudTypes.slang"
struct CloudFrameParameters { CloudParameters model; CloudRayData origin; float4 forward,right,up,clear; };
[[vk::binding(10,0)]] ConstantBuffer<CloudFrameParameters> cloudFrame;
#define cp cloudFrame.model
struct ReconstructionParameters {
    uint4 size; // half width/height, valid completed history, empty-space skipping
    float4 controls; // termination T, history weight, unused, unused
    float4 previousClip[4]; // Current anchor-relative point, wind compensated, to previous clip.
    float4 cameraDelta; // Current minus previous anchor, without wind compensation.
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
#if DK_CLOUD_STAGE == 2
[[vk::binding(9,0)]] Texture2D<float4> surfaceColor;
#endif
float3 worldRay(float2 pixel) {
    float2 screen=(pixel+.5)/float2(p.frame.xy)*2-1;
    return normalize(cloudFrame.forward.xyz+screen.x*cloudFrame.right.xyz+screen.y*cloudFrame.up.xyz);
}
bool matchingDepth(float a,float b) {
    return (a<0 && b<0) || (a>=0 && b>=0 && abs(a-b)<=max(2,.02*max(a,b)));
}
#if DK_CLOUD_STAGE != 1
float3 localRay(float2 pixel) {
    float2 screen=(pixel+.5)/float2(p.frame.xy)*2-1;
    return p.forward.xyz+screen.x*p.right.xyz+screen.y*p.up.xyz;
}
float geometryDistance(uint2 pixel) {
    float depth=sceneDepth.Load(int3(pixel,0));
    return depth<1 ? p.depth.y/(depth+p.depth.x)*length(localRay(pixel))*1000 : -1;
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
struct Medium { float4 L,T,cloud; };
Medium trace(uint2 pixel) {
    float3 viewRay=localRay(pixel),direction=normalize(viewRay);
    CloudRayData ray=cloudFrame.origin; ray.direction.xyz=worldRay(pixel);
    float mu=direction.y,cosine=clamp(dot(direction,p.sunDirection.xyz),-1,1);
    ray.geometry.y=mu; ray.geometry.w=cosine;
    float geometry=geometryDistance(pixel); float3 segment=path(p.geometry.z,mu);
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
                ++lighting; float tau=cloudSunlightOptical(ray,ray.direction.xyz*t,h,sunMu,abs(sunMu)<.2 ? 16u : 0u);
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
    uint priorSamples; InterlockedAdd(statistics[8],densitySamples,priorSamples);
    if(priorSamples>0xffffffffu-densitySamples) InterlockedAdd(statistics[9],1);
    Medium value; value.L=float4(L,geometry); value.T=float4(T,opacity>0 ? moment/opacity : 0); value.cloud=float4(cloudL,cloudT); return value;
}
#endif
[numthreads(8,8,1)]
void computeMain(uint3 id:SV_DispatchThreadID) {
#if DK_CLOUD_STAGE == 0
    if(any(id.xy>=reconstruction.size.xy)) return;
    Medium value=trace(min(id.xy*2,p.frame.xy-1)); outputL[id.xy]=value.L; outputT[id.xy]=value.T; outputCloud[id.xy]=value.cloud;
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
    float3 direction=worldRay(min(id.xy*2,p.frame.xy-1)); float4 position=float4(direction*T.a,1);
    float4 clip=float4(dot(reconstruction.previousClip[0],position),dot(reconstruction.previousClip[1],position),
        dot(reconstruction.previousClip[2],position),dot(reconstruction.previousClip[3],position));
    float2 oldPixel=(clip.xy/max(clip.w,1e-8)*.5+.5)*float2(p.frame.xy)-.5;
    float2 old=oldPixel*.5; int2 base=int2(floor(old)); float2 fraction=frac(old);
    bool accepted=reconstruction.size.z!=0 && !edge && T.a>0 && C.a<.999 && clip.w>0 && all(old>=0) && all(old<=float2(reconstruction.size.xy)-1);
    float4 priorL=0,priorT=0,priorC=0;
    if(accepted) {
        float predicted=L.a<0 ? -1 : length(reconstruction.cameraDelta.xyz+direction*L.a);
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
    float geometry=geometryDistance(id.xy); float2 halfPixel=float2(id.xy)*.5;
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
    if(any((id.xy&1u)!=0)) for(int y=-1;y<=1;++y) for(int x=-1;x<=1;++x) {
        int2 point=clamp(base+int2(x,y),0,int2(reconstruction.size.xy)-1);
        float4 a=currentL.Load(int3(point,0)),b=currentT.Load(int3(point,0)),c=currentCloud.Load(int3(point,0));
        if(!matchingDepth(geometry,a.a)) total=0;
        minT=min(minT,b.rgb); maxT=max(maxT,b.rgb); minC=min(minC,c.a); maxC=max(maxC,c.a);
    }
    float3 direction=normalize(localRay(id.xy)),sunDelta=direction-p.sunDirection.xyz;
    bool solar=p.frame.w!=0 && dot(sunDelta,sunDelta)<=p.sunDirection.w;
    if(total<.999 || solar || max(maxC-minC,max(maxT.x-minT.x,max(maxT.y-minT.y,maxT.z-minT.z)))>.02) {
        Medium value=trace(id.xy); L=value.L.rgb; T=value.T.rgb; C=value.cloud; representative=value.T.a;
        InterlockedAdd(statistics[5],1);
    } else { L/=total; T/=total; C/=total; representative/=total; InterlockedAdd(statistics[4],1); }
    float3 segment=path(p.geometry.z,direction.y),terminal=0;
    float distance=geometry>=0 ? geometry : max(0,segment.y*1000);
    if(!(segment.z>0 && distance>=segment.y*1000)) {
        if(geometry>=0) terminal=surfaceColor.Load(int3(id.xy,0)).rgb;
        else {
            if(cp.flags.y==0) terminal=cloudFrame.clear.rgb;
            float3 delta=direction-p.sunDirection.xyz;
            if(p.frame.w!=0 && dot(delta,delta)<=p.sunDirection.w) terminal+=p.sun.rgb*p.sun.w;
        }
    }
    outputL[id.xy]=float4(L+T*terminal,1); outputT[id.xy]=C; outputCloud[id.xy]=float4(T,representative);
#endif
}
