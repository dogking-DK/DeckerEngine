// Model 4, sampler 2: scalar hero-RGB analog transport. No view-dependent LUTs.
struct AirLocation { float3 radial; float height; };
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
    uint previous; InterlockedAdd(status[1],1,previous);
    uint4 limits=asuint(parameters[12]);
    if(steps>=limits.z || previous>=limits.w) { airError(2); return false; }
    ++steps; return true;
}
AirLocation airMove(AirLocation a,float3 delta) {
    if(asuint(parameters[12]).y==0) return a;
    float R=parameters[14].x,r=R+a.height,change=dot(delta,delta)+2*r*dot(a.radial,delta);
    AirLocation b; b.height=(a.height*(2*R+a.height)+change)/(sqrt(max(0.0,r*r+change))+R);
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
    if(asuint(parameters[12]).y==0 || height>=parameters[14].y) return 0;
    float ray=parameters[15][channel]*exp(-height/parameters[15].w);
    float mie=parameters[16][channel]*exp(-height/parameters[16].w);
    float extinction=ray+parameters[17][channel]*exp(-height/parameters[16].w)+
        parameters[18][channel]*max(0.0,1-abs(height-parameters[18].w)/parameters[19].w);
    if(!isfinite(extinction) || extinction<0 || extinction>parameters[20][channel]) airError(8);
    return float3(ray,mie,extinction);
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
float airPhase(float cosine,float rayWeight) {
    cosine=clamp(cosine,-1.0,1.0); float g=parameters[17].w;
    return rayWeight*3*(1+cosine*cosine)/(16*pi)+(1-rayWeight)*(1-g*g)/(4*pi*pow(1+g*g-2*g*cosine,1.5));
}
float3 airPhaseSample(float3 incoming,float rayWeight,uint pixel,uint sample,uint event) {
    float u=airRandom(pixel,sample,event,5),cosine;
    if(airRandom(pixel,sample,event,4)<rayWeight) {
        float q=4*u-2,r=pow(q+sqrt(q*q+1),1.0/3.0); cosine=r-1/r;
    } else {
        float g=parameters[17].w;
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
    float rayWeight;
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
    vertex=(AirVertex)0; float rate=parameters[20][channel]; if(rate==0) return 0;
    float t=segment.x,correction=0; uint counter=0;
    while(t<segment.y) {
        if(!airStep(steps)) return 3;
        uint at=counter++; float optical=airOpticalDistance(airRandom(pixel,sample,event,2,at));
        if(optical>=(segment.y-t)*rate) break; // Do not overflow a free path beyond the shell.
        float adjusted=optical/rate-correction,next=t+adjusted;
        correction=(next-t)-adjusted; // Retain steps below the current distance's float ulp.
        if(next<t || !isfinite(next)) { airError(4); return 3; }
        t=next; if(t>=segment.y) break;
        AirLocation point=airAlong(location,ray.Direction,t); float3 c=airCoefficients(point.height,channel);
        if(status[0]!=0) return 3;
        float choice=airRandom(pixel,sample,event,3,at)*rate,scattering=c.x+c.y;
        if(choice<scattering) {
            vertex.volume=true; vertex.position=ray.Origin+ray.Direction*t; vertex.incoming=ray.Direction;
            vertex.location=point; vertex.rayWeight=c.x/scattering; return 1;
        }
        if(choice<c.z) return 2;
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
    float rate=parameters[20][channel]; if(rate==0) return 1;
    float t=segment.x,T=1,correction=0; uint counter=0;
    while(t<segment.y) {
        if(!airStep(steps)) return 0;
        float optical=airOpticalDistance(airRandom(pixel,sample,event,10,counter++));
        if(optical>=(segment.y-t)*rate) break;
        float adjusted=optical/rate-correction,next=t+adjusted; correction=(next-t)-adjusted;
        if(next<t || !isfinite(next)) { airError(4); return 0; }
        t=next; if(t>=segment.y) break;
        T*=1-airCoefficients(airAlong(location,ray.Direction,t).height,channel).z/rate;
        if(T==0 || status[0]!=0) return 0;
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
    if(v.volume) { factor=airPhase(dot(v.incoming,wi),v.rayWeight); scatterPdf=factor; }
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
        if(v.volume) { wi=airPhaseSample(ray.Direction,v.rayWeight,pixel,sample,depth); previousPdf=airPhase(dot(ray.Direction,wi),v.rayWeight); }
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
    if(status[0]!=0) return asfloat(0x7fc00000u).xxx;
    float3 result=0; result[channel]=3*value; return result;
}
