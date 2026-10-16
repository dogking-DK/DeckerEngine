// Model 4, sampler 2: scalar hero-RGB analog transport. No view-dependent LUTs.
struct AirLocation { float3 radial; float height; float2 weather; float3 base,detail; };
#include "path-cloud.slang"
float airRandom(uint pixel,uint sample,uint event,uint stream,uint counter=0) {
    uint4 p=asuint(parameters[1]); uint h=mixBits(p.x^0x3c6ef372u);
    h=mixBits(h^p.y); h=mixBits(h^pixel); h=mixBits(h^sample); h=mixBits(h^event); h=mixBits(h^stream); h=mixBits(h^counter);
    return min((float(h>>8)+.5)/16777216.0,asfloat(0x3f7fffff));
}
void airError(uint code) { uint ignored; InterlockedOr(status[0],code,ignored); }
float airOpticalDistance(float u) {
    // Preserve the smallest sampler values when float subtraction would round 1-u to 1.
    return u<.001 ? u*(1+u*(.5+u/3)) : -log(1-u);
}
bool airStep(inout uint steps) {
    if(status[0]!=0) return false;
    uint4 limits=asuint(parameters[12]);
    if(steps>=limits.z) { airError(2); return false; }
    ++steps; return true;
}
// Merge once per path. The per-path cap bounds in-flight work; the tile cap
// rejects the entire candidate before publication. The first excess is detected
// without overflowing: limit <= 2^28 and steps <= 2^20. Any later wrap belongs
// to an already rejected tile; the sticky error cannot be cleared by another ray.
void airCommitSteps(uint steps) {
    if(steps==0 || status[0]!=0) return;
    uint limit=asuint(parameters[12]).w,previous;
    InterlockedAdd(status[1],steps,previous);
    if(previous>limit || steps>limit-previous) airError(2);
}
AirLocation airMove(AirLocation a,float3 delta) {
    AirLocation b=a;
    if(asuint(parameters[22]).y!=0) {
        b.weather=float2(cloudShift(a.weather.x,delta.x,0),cloudShift(a.weather.y,delta.z,0));
        [unroll] for(uint i=0;i<3;++i) { b.base[i]=cloudShift(a.base[i],delta[i],1); b.detail[i]=cloudShift(a.detail[i],delta[i],2); }
    }
    if(asuint(parameters[12]).y==0) return b;
    float R=parameters[14].x,r=R+a.height,change=dot(delta,delta)+2*r*dot(a.radial,delta);
    b.height=(a.height*(2*R+a.height)+change)/(sqrt(max(0.0,r*r+change))+R);
    b.radial=normalize(a.radial*r+delta); return b;
}
AirLocation airAlong(AirLocation a,float3 direction,float distance) {
    AirLocation b=airMove(a,direction*distance);
    // Known in-shell samples can round a few ulps below the analytically clipped ground.
    if(b.height < -max(.001,8*1.1920929e-7*(abs(a.height)+distance))) airError(4);
    b.height=max(0.0,b.height); return b;
}
float2 airRoots(AirLocation a,float3 direction,float boundary) {
    float R=parameters[14].x,b=(R+a.height)*dot(a.radial,direction);
    float c=(a.height-boundary)*(2*R+a.height+boundary),discriminant=b*b-c;
    if(discriminant<0) return float2(1,-1);
    float q=-b-(b>=0 ? 1 : -1)*sqrt(discriminant);
    if(q==0) return 0;
    return float2(min(q,c/q),max(q,c/q));
}
float3 airSegment(AirLocation a,float3 direction,float maximum) {
    if(asuint(parameters[12]).y==0) return 0;
    if(!isfinite(a.height) || a.height<0) { airError(4); return 0; }
    float2 top=airRoots(a,direction,parameters[14].y);
    if(top.x>top.y || top.y<0) return 0;
    float3 segment=float3(max(0.0,top.x),min(maximum,top.y),0);
    float2 ground=airRoots(a,direction,0);
    if(dot(a.radial,direction)<0 && ground.x<=ground.y && ground.x>=0 && ground.x<=segment.y) {
        segment.y=ground.x; segment.z=1;
    }
    if(segment.y>2e7) airError(4);
    return segment;
}
float3 airCoefficients(float height,uint channel) {
    if(asuint(parameters[22]).x==0 || height>=parameters[14].y) return 0;
    float ray=parameters[15][channel]*exp(-height/parameters[15].w);
    float mie=parameters[16][channel]*exp(-height/parameters[16].w);
    float extinction=ray+parameters[17][channel]*exp(-height/parameters[16].w)+
        parameters[18][channel]*max(0.0,1-abs(height-parameters[18].w)/parameters[19].w);
    if(!isfinite(extinction) || extinction<0 || extinction>parameters[20][channel]) airError(8);
    return float3(ray,mie,extinction);
}
struct AirIntervals { float3 segment[5]; uint count; };
AirIntervals airIntervals(AirLocation a,float3 direction,float3 bounds) {
    AirIntervals result=(AirIntervals)0; if(bounds.y<=bounds.x) return result;
    float cuts[6]; uint count=2; cuts[0]=bounds.x; cuts[1]=bounds.y;
    bool clouds=asuint(parameters[22]).y!=0;
    if(clouds) [unroll] for(uint boundary=0;boundary<2;++boundary) {
        float2 roots=airRoots(a,direction,parameters[23][boundary]);
        if(roots.x<roots.y) [unroll] for(uint i=0;i<2;++i) if(roots[i]>bounds.x && roots[i]<bounds.y) cuts[count++]=roots[i];
    }
    for(uint i=1;i<count;++i) { float key=cuts[i]; uint j=i; while(j>0 && cuts[j-1]>key) { cuts[j]=cuts[j-1]; --j; } cuts[j]=key; }
    for(uint i=1;i<count;++i) if(cuts[i]>cuts[i-1]) {
        float height=airMove(a,direction*((cuts[i]+cuts[i-1])*.5)).height;
        bool inside=clouds && height>parameters[23].x && height<parameters[23].y;
        if(inside || asuint(parameters[22]).x!=0) result.segment[result.count++]=float3(cuts[i-1],cuts[i],inside ? 1 : 0);
    }
    return result;
}
float4 airMediumCoefficients(AirLocation a,uint channel,float rate) {
    float3 air=airCoefficients(a.height,channel); float cloud=cloudDensity(a)*parameters[24].x;
    float4 c=float4(air.xy,cloud*parameters[24].y,air.z+cloud);
    if(any(!isfinite(c)) || any(c<0) || c.w>rate) airError(8);
    return c;
}
float3 airTangent(float3 axis) {
    // Match the CPU reference's unitOrthogonal frame (double dummy precision 1e-12).
    return abs(axis.x)>1e-12*abs(axis.z) || abs(axis.y)>1e-12*abs(axis.z) ?
        normalize(float3(-axis.y,axis.x,0)) : normalize(float3(0,-axis.z,axis.y));
}
float3 airDirection(float3 axis,float cosine,float azimuth) {
    float3 x=airTangent(axis),y=cross(axis,x);
    float radius=sqrt(max(0.0,1-cosine*cosine)),phi=2*pi*azimuth;
    return normalize(axis*cosine+radius*(x*cos(phi)+y*sin(phi)));
}
float3 airSunDirection(float u,float v) {
    float3 axis=normalize(parameters[5].xyz),x=airTangent(axis),y=cross(axis,x);
    float w=parameters[21].x*u,phi=2*pi*v;
    return normalize(axis*(1-w)+sqrt(w*(2-w))*(x*cos(phi)+y*sin(phi)));
}
bool airSunHit(float3 direction) {
    float3 axis=normalize(parameters[5].xyz),crossed=cross(direction,axis);
    return dot(direction,axis)>0 && dot(crossed,crossed)<=parameters[21].z;
}
float airHG(float cosine,float g) { return (1-g*g)/(4*pi*pow(1+g*g-2*g*cosine,1.5)); }
float airPhase(float cosine,float3 weights) {
    cosine=clamp(cosine,-1.0,1.0);
    return dot(weights,float3(3*(1+cosine*cosine)/(16*pi),airHG(cosine,parameters[17].w),airHG(cosine,parameters[24].z)));
}
float3 airPhaseSample(float3 incoming,float3 weights,uint pixel,uint sample,uint event) {
    float u=airRandom(pixel,sample,event,5),cosine,choice=airRandom(pixel,sample,event,4);
    if(choice<weights.x) {
        float q=4*u-2,r=pow(q+sqrt(q*q+1),1.0/3.0); cosine=r-1/r;
    } else {
        float g=choice<weights.x+weights.y ? parameters[17].w : parameters[24].z;
        if(abs(g)<.001) { float w=2*u-1; cosine=(2*w+g*(w*w+3)+2*g*g*w+g*g*g*(w*w-1))/(2*(1+g*w)*(1+g*w)); }
        else { float q=(1-g*g)/(1-g+2*g*u); cosine=(1+g*g-q*q)/(2*g); }
    }
    return airDirection(incoming,clamp(cosine,-1.0,1.0),airRandom(pixel,sample,event,5,1));
}
PathPayload airNearest(RayDesc ray,inout uint rays) {
    PathPayload hit={}; ++rays;
#if !DK_PATH_EMPTY
    TraceRay(scene,RAY_FLAG_NONE,255,0,2,0,ray,hit);
#endif
    return hit;
}
struct AirVertex {
    float3 position,normal,incoming;
    AirLocation location;
    PathBsdf bsdf;
    uint index;
    bool volume,ground;
    float3 phaseWeights;
};
float3 airOrigin(AirVertex vertex,float3 direction,out AirLocation location) {
    location=vertex.location; if(vertex.volume) return vertex.position;
    float3 origin=vertex.position;
#if !DK_PATH_EMPTY
    if(!vertex.ground) origin=offsetOrigin(vertex.index,origin,direction);
#endif
    location=airMove(location,origin-vertex.position);
    if(asuint(parameters[12]).y!=0 && location.height<.0001) {
        float shift=.0001-location.height; origin+=location.radial*shift; location.height=.0001;
    }
    return origin;
}
// 0 endpoint, 1 scatter, 2 absorb, 3 error. Null collisions do not increment physical depth.
uint airFlight(RayDesc ray,AirLocation location,float3 segment,uint channel,uint pixel,uint sample,uint event,inout uint steps,out AirVertex vertex) {
    vertex=(AirVertex)0; uint counter=0; AirIntervals intervals=airIntervals(location,ray.Direction,segment);
    for(uint k=0;k<intervals.count;++k) {
        float3 interval=intervals.segment[k]; float rate=interval.z!=0 ? parameters[30][channel] : parameters[20][channel];
        if(rate==0) continue;
        float t=interval.x,correction=0;
        while(t<interval.y) {
            if(!airStep(steps)) return 3;
            uint at=counter++; float optical=airOpticalDistance(airRandom(pixel,sample,event,2,at));
            if(optical>=(interval.y-t)*rate) break;
            float adjusted=optical/rate-correction,next=t+adjusted; correction=(next-t)-adjusted;
            if(next<t || !isfinite(next)) { airError(4); return 3; }
            t=next; if(t>=interval.y) break;
            AirLocation point=airAlong(location,ray.Direction,t); float4 c=airMediumCoefficients(point,channel,rate);
            if(status[0]!=0) return 3;
            float choice=airRandom(pixel,sample,event,3,at)*rate,scattering=c.x+c.y+c.z;
            if(choice<scattering) {
                vertex.volume=true; vertex.position=ray.Origin+ray.Direction*t; vertex.incoming=ray.Direction;
                vertex.location=point; vertex.phaseWeights=c.xyz/scattering; return 1;
            }
            if(choice<c.w) return 2;
        }
    }
    return 0;
}

float airVisibility(AirVertex v,float3 wi,uint light,float3 point,uint channel,uint pixel,uint sample,uint event,inout uint rays,inout uint steps) {
    RayDesc ray; AirLocation location; ray.Origin=airOrigin(v,wi,location); ray.Direction=wi; ray.TMin=0; ray.TMax=infinity;
#if !DK_PATH_EMPTY
    if(light!=0xffffffffu) {
        float3 end=offsetOrigin(light,point,-wi),delta=end-ray.Origin; float distance=length(delta);
        if(distance<=0 || dot(delta,wi)<=0) return 0;
        ray.Direction=delta/distance; ray.TMax=nextFloat(distance,-1);
    }
#endif
    if(airNearest(ray,rays).found!=0) return 0;
    float3 segment=airSegment(location,ray.Direction,ray.TMax); if(segment.z!=0) return 0;
    float T=1; uint counter=0; AirIntervals intervals=airIntervals(location,ray.Direction,segment);
    for(uint k=0;k<intervals.count;++k) {
        float3 interval=intervals.segment[k]; float rate=interval.z!=0 ? parameters[30][channel] : parameters[20][channel];
        if(rate==0) continue;
        float t=interval.x,correction=0;
        while(t<interval.y) {
            if(!airStep(steps)) return 0;
            float optical=airOpticalDistance(airRandom(pixel,sample,event,10,counter++));
            if(optical>=(interval.y-t)*rate) break;
            float adjusted=optical/rate-correction,next=t+adjusted; correction=(next-t)-adjusted;
            if(next<t || !isfinite(next)) { airError(4); return 0; }
            t=next; if(t>=interval.y) break;
            float extinction=airMediumCoefficients(airAlong(location,ray.Direction,t),channel,rate).w;
            if(status[0]!=0) return 0;
            T*=1-extinction/rate; if(T==0) return 0;
        }
    }
    return T;
}

float airDirect(AirVertex v,uint channel,uint pixel,uint sample,uint event,inout uint rays,inout uint steps) {
    uint4 flags=asuint(parameters[2]); float classPdf=parameters[4].w;
    if(flags.x!=0 || classPdf==0) return 0;
    int selected=int(airRandom(pixel,sample,event,6)/classPdf);
    float3 wi=0,point=0; float radiance=0,pdf=0; uint light=0xffffffffu;
    if(largest(parameters[3].xyz)>0 && selected--==0) {
        wi=airDirection(float3(0,1,0),1-2*airRandom(pixel,sample,event,7),airRandom(pixel,sample,event,7,1));
        radiance=parameters[3][channel]; pdf=classPdf/(4*pi);
    } else if(flags.y!=0 && selected--==0) {
        wi=airSunDirection(airRandom(pixel,sample,event,7),airRandom(pixel,sample,event,7,1));
        radiance=parameters[4][channel]; pdf=classPdf*parameters[21].y;
    }
#if !DK_PATH_EMPTY
    else if(parameters[3].w>0) {
        float target=airRandom(pixel,sample,event,7)*parameters[3].w; uint lo=0,hi=flags.z;
        while(lo<hi) { uint mid=(lo+hi)/2; if(target<triangles[mid*9+2].w) hi=mid; else lo=mid+1; }
        if(lo==flags.z) return 0; light=lo;
        float root=sqrt(airRandom(pixel,sample,event,7,1)),u=airRandom(pixel,sample,event,7,2);
        point=triangles[light*9].xyz*(1-root)+triangles[light*9+1].xyz*(root*(1-u))+triangles[light*9+2].xyz*(root*u);
        float3 delta=point-v.position; if(dot(delta,delta)==0) return 0;
        wi=normalize(delta); radiance=triangles[light*9+5][channel]; pdf=areaPdf(light,v.position,point);
    }
#endif
    if(pdf<=0 || radiance==0) return 0;
    float factor,scatterPdf;
    if(v.volume) { factor=airPhase(dot(v.incoming,wi),v.phaseWeights); scatterPdf=factor; }
    else { BsdfEval b=evalBsdf(v.bsdf,v.normal,-v.incoming,wi); factor=b.value[channel]*max(0.0,dot(v.normal,wi)); scatterPdf=b.pdf; }
    if(factor<=0) return 0;
    return radiance*factor*airVisibility(v,wi,light,point,channel,pixel,sample,event,rays,steps)*weight(pdf,scatterPdf)/pdf;
}
float3 traceAir(RayDesc ray,AirLocation location,uint pixel,uint sample,inout uint rays,out float4 firstHit,out float4 firstNormal) {
    uint4 counts=asuint(parameters[1]),flags=asuint(parameters[2]); uint channel=min(2u,uint(airRandom(pixel,sample,0,0)*3)),steps=0;
    float value=0,beta=1,previousPdf=0; float3 previous=0;
    firstHit=float4(-1,0,0,0); firstNormal=0;
    for(uint depth=0;depth<=counts.z;++depth) {
        if(status[0]!=0) break;
        PathPayload hit=airNearest(ray,rays);
        float3 segment=airSegment(location,ray.Direction,hit.found!=0 ? hit.distance : infinity);
        AirVertex v; uint event=airFlight(ray,location,segment,channel,pixel,sample,depth,steps,v);
        if(event==2 || event==3) break;
        if(event==0) {
            bool ground=segment.z!=0 && (hit.found==0 || segment.y<hit.distance);
            if(!ground && hit.found==0) {
                value+=beta*parameters[3][channel]*(depth!=0 && flags.x==0 ? weight(previousPdf,parameters[4].w/(4*pi)) : 1);
                if(flags.y!=0 && airSunHit(ray.Direction)) value+=beta*parameters[4][channel]*(depth!=0 && flags.x==0 ? weight(previousPdf,parameters[4].w*parameters[21].y) : 1);
                break;
            }
            v.ground=ground; v.incoming=ray.Direction;
            if(ground) {
                v.position=ray.Origin+ray.Direction*segment.y; v.location=airMove(location,ray.Direction*segment.y); v.location.height=0;
                v.normal=v.location.radial; v.bsdf.color=parameters[19].xyz; v.bsdf.metallic=0; v.bsdf.roughness=1; v.bsdf.ggx=false;
            }
#if !DK_PATH_EMPTY
            else {
                v.index=hit.index; v.position=positionOf(hit); v.normal=normalOf(hit,ray.Direction); v.bsdf=materialAt(hit.index,hit.barycentric);
                v.location=airAlong(location,ray.Direction,hit.distance);
                value+=beta*triangles[hit.index*9+5][channel]*(depth!=0 && flags.x==0 ? weight(previousPdf,areaPdf(hit.index,previous,v.position)) : 1);
                if(depth==0) { firstHit=float4(hit.distance,hit.barycentric,float(hit.index+1)); firstNormal=float4(v.normal,dot(ray.Direction,triangles[hit.index*9+3].xyz)<0 ? 1 : -1); }
            }
#endif
        }
        if(depth==counts.z) break;
        value+=beta*airDirect(v,channel,pixel,sample,depth,rays,steps);
        float3 wi;
        if(v.volume) { wi=airPhaseSample(ray.Direction,v.phaseWeights,pixel,sample,depth); previousPdf=airPhase(dot(ray.Direction,wi),v.phaseWeights); }
        else {
            wi=sampleBsdf(v.bsdf,v.normal,-ray.Direction,airRandom(pixel,sample,depth,8),airRandom(pixel,sample,depth,8,1));
            BsdfEval evaluated=evalBsdf(v.bsdf,v.normal,-ray.Direction,wi); if(evaluated.pdf<=0) break;
            previousPdf=evaluated.pdf; beta*=evaluated.value[channel]*max(0.0,dot(v.normal,wi))/previousPdf;
        }
        if(!isfinite(beta) || beta<0) { airError(1); break; } if(beta==0) break;
        if(counts.w!=0 && depth+1>=counts.w) {
            float survival=clamp(beta,.05,.95); if(airRandom(pixel,sample,depth,9)>=survival) break; beta/=survival;
        }
        previous=v.position; ray.Origin=airOrigin(v,wi,location); ray.Direction=wi; ray.TMin=0; ray.TMax=infinity;
    }
    airCommitSteps(steps);
    if(status[0]!=0) return asfloat(0x7fc00000u).xxx;
    float3 result=0; result[channel]=3*value; return result;
}
