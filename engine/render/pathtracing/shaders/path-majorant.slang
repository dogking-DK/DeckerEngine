// Conservative piecewise bounds for the same continuous medium. No sampled-point maxima.
struct MediumMajorant { float end,rate; };
#if DK_PATH_LOCAL_MAJORANTS
float majorantHeight(AirLocation a,float3 direction,float t) {
    float R=parameters[14].x,r=R+a.height,change=t*(t+2*r*dot(a.radial,direction));
    return (a.height*(2*R+a.height)+change)/(sqrt(max(0.0,r*r+change))+R);
}
float2 majorantHeights(AirLocation a,float3 direction,float begin,float end) {
    float closest=clamp(-(parameters[14].x+a.height)*dot(a.radial,direction),begin,end);
    float low=majorantHeight(a,direction,closest);
    float high=max(majorantHeight(a,direction,begin),majorantHeight(a,direction,end));
    float pad=max(.01,32*1.1920929e-7*(abs(a.height)+max(abs(begin),abs(end))));
    return float2(max(0,low-pad),high+pad);
}
float majorantAir(float2 height,uint channel) {
    if(asuint(parameters[22]).x==0 || height.x>=parameters[14].y) return 0;
    float absorber=max(0.0,1-abs(parameters[18].w-clamp(parameters[18].w,height.x,height.y))/parameters[19].w);
    return parameters[15][channel]*exp(-height.x/parameters[15].w)+
        parameters[17][channel]*exp(-height.x/parameters[16].w)+parameters[18][channel]*absorber;
}
float majorantExit(float3 cell,float3 velocity) {
    float3 remaining=select(velocity>=0,1-frac(cell),frac(cell));
    float3 distance=select(abs(velocity)>1e-20,remaining/max(abs(velocity),1e-20),float3(1e30));
    return min(distance.x,min(distance.y,distance.z));
}
MediumMajorant mediumMajorant(AirLocation origin,float3 direction,float begin,float end,bool cloudy,uint channel) {
    MediumMajorant result; result.end=end; result.rate=cloudy ? parameters[30][channel] : parameters[20][channel];
    // Short mesh/NEE segments already need fewer than two expected proposals.
    // The existing global bound avoids paying spatial-bound setup for these rays.
    if((end-begin)*result.rate<2) return result;
    float baseMax=0; float2 weatherMax=0;
    if(cloudy) {
        uint4 sizes=asuint(parameters[22]); uint edge=uint(parameters[14].w);
        uint ws=max(1u,sizes.y/edge),bs=max(1u,sizes.z/edge);
        float fine=min(1/(parameters[25].x*sizes.y),1/(parameters[25].y*sizes.z));
        // Retain the global oracle when distance/phase roundoff could consume the halo.
        if(32*1.1920929e-7*max(abs(begin),abs(end))>fine*.125) return result;
        AirLocation point=airAlong(origin,direction,begin);
        float2 wc=point.weather*ws; float3 bc=point.base*bs;
        float weatherExit=majorantExit(float3(wc,0),float3(direction.xz*parameters[25].x*ws,0));
        uint2 wi=min(uint2(floor(wc)),ws-1);
        uint offset=2*sizes.y*sizes.y+sizes.z*sizes.z*sizes.z+sizes.w*sizes.w*sizes.w;
        uint wat=offset+2*(wi.x+ws*wi.y);
        weatherMax=float2(cloudByte(wat),cloudByte(wat+1))+1e-5;
        // Empty weather cells need no base-grid traversal. Both grids retain their repeat halo.
        bool empty=weatherMax.x<=1-parameters[23].z;
        float exit=empty ? weatherExit : min(weatherExit,majorantExit(bc,direction*parameters[25].y*bs));
        result.end=min(end,begin+max(exit,fine*.125));
        if(!(result.end>begin)) { result.end=end; return result; }
        if(empty) cloudy=false;
        else {
            uint3 bi=min(uint3(floor(bc)),bs-1);
            uint bat=offset+2*ws*ws+bi.x+bs*(bi.y+bs*bi.z);
            baseMax=cloudByte(bat)+1e-5;
        }
    }
    float2 heights=majorantHeights(origin,direction,begin,result.end);
    float bound=majorantAir(heights,channel);
    if(cloudy) {
        float4 shell=parameters[23]; float2 h=(heights-shell.x)/(shell.y-shell.x);
        float coverage=saturate((weatherMax.x-(1-shell.z))/shell.z);
        float profile=cloudSmooth(h.y/parameters[25].w)*cloudSmooth((.6+.4*weatherMax.y-h.x)/parameters[24].w)+1e-5;
        float density=shell.w*saturate((baseMax*profile-(1-coverage))/.35+1e-5);
        bound+=density*parameters[24].x;
    }
    if(bound>0) bound=asfloat(asuint(bound*(1+1e-5))+16u);
    result.rate=min(result.rate,bound); return result;
}

#endif
