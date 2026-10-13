static const float PI=3.14159265358979323846;
float2 roots(float h,float mu,float boundary) {
    float b=(p.geometry.x+h)*mu;
    float c=(h-boundary)*(2*p.geometry.x+h+boundary);
    float discriminant=b*b-c;
    if(discriminant<0) return float2(1,-1);
    float q=-b-(b>=0 ? 1 : -1)*sqrt(discriminant);
    if(q==0) return 0;
    float other=c/q;
    return float2(min(q,other),max(q,other));
}
float3 path(float h,float mu) {
    float2 top=roots(h,mu,p.geometry.y);
    if(top.y<0 || top.x>top.y) return 0;
    float3 result=float3(max(0,top.x),top.y,0);
    float2 groundHit=roots(h,mu,0);
    if(mu<0 && groundHit.x<=groundHit.y && groundHit.x>=0 && groundHit.x<=result.y) { result.y=groundHit.x; result.z=1; }
    return result;
}
float heightAt(float h,float mu,float t) {
    float r=p.geometry.x+h, delta=t*(t+2*r*mu);
    return max(0,h+delta/(sqrt(max(0,r*r+delta))+r));
}
float node(uint i,uint count) { return .5-.5*cos(PI*float(i)/float(count)); }
void medium(float h,out float3 ray,out float3 mie,out float3 ext) {
    ray=p.rayleigh.rgb*exp(-h/p.rayleigh.w);
    mie=p.mie.rgb*exp(-h/p.mie.w);
    ext=ray+p.extinction.rgb*exp(-h/p.mie.w)+p.absorption.rgb*max(0,1-abs(h-p.absorption.w)/p.ground.w);
}
float3 weight(float3 ext,float ds) {
    float3 optical=ext*ds;
    // Avoid cancellation and 0/0 in vacuum (including eager vector selection).
    return ds*select(optical<.001,1-optical*.5+optical*optical/6,(1-exp(-optical))/max(optical,1e-20));
}
float3 opticalTransmittance(float h,float mu,float begin,float end,uint count) {
    float3 optical=0;
    [loop] for(uint i=0;i<count;++i) {
        float a=lerp(begin,end,node(i,count)),b=lerp(begin,end,node(i+1,count));
        float3 ray,mie,ext; medium(heightAt(h,mu,(a+b)*.5),ray,mie,ext);
        optical+=ext*(b-a);
    }
    return exp(-optical);
}
float3 directTransmittance(float h,float mu) {
    float3 segment=path(h,mu);
    if(segment.z>0) return 0;
    return opticalTransmittance(h,mu,segment.x,segment.y,512);
}
float2 endpointUv(float2 uv,float2 size) { return (saturate(uv)*(size-1)+.5)/size; }
#if DK_STAGE > 0
float3 sunlight(float h,float mu) {
    float3 segment=path(h,mu);
    if(segment.z>0) return 0;
    float H=sqrt(p.geometry.y*(2*p.geometry.x+p.geometry.y));
    float rho=sqrt(max(0,h*(2*p.geometry.x+h)));
    float dmin=p.geometry.y-h, dmax=rho+H;
    float2 uv=float2((segment.y-dmin)/max(dmax-dmin,1e-8),rho/H);
    return transmittanceLut.SampleLevel(linearClamp,endpointUv(uv,float2(256,64)),0).rgb;
}
#endif
#if DK_STAGE > 1
float3 multiple(float h,float mu) {
    return multipleLut.SampleLevel(linearClamp,endpointUv(float2(mu*.5+.5,sqrt(saturate(h/p.geometry.y))),float2(32,32)),0).rgb;
}
void integrateSegment(float h,float mu,float cosine,float sunMu,uint count,bool reference,float limit,
    out float3 radiance,out float3 throughput) {
    float3 segment=path(h,mu); throughput=1; radiance=0;
    segment.y=min(segment.y,limit);
    if(segment.y<=segment.x) return;
    float rayPhase=3*(1+cosine*cosine)/(16*PI);
    float g=p.extinction.w;
    float miePhase=(1-g*g)/(4*PI*pow(1+g*g-2*g*cosine,1.5));
    [loop] for(uint i=0;i<count;++i) {
        float a=lerp(segment.x,segment.y,node(i,count)),b=lerp(segment.x,segment.y,node(i+1,count)),t=(a+b)*.5;
        float sh=heightAt(h,mu,t);
        float sm=clamp(((p.geometry.x+h)*sunMu+t*cosine)/(p.geometry.x+sh),-1,1);
        float3 ray,mie,ext; medium(sh,ray,mie,ext);
        float3 source=(ray*rayPhase+mie*miePhase)*(reference ? directTransmittance(sh,sm) : sunlight(sh,sm));
        if(p.up.w>0) source+=(ray+mie)*multiple(sh,sm);
        radiance+=throughput*source*weight(ext,b-a);
        throughput*=exp(-ext*(b-a));
    }
}
float3 integrate(float h,float mu,float cosine,float sunMu,uint count,bool reference) {
    float3 radiance,throughput;
    integrateSegment(h,mu,cosine,sunMu,count,reference,1e20,radiance,throughput);
    return radiance;
}
float horizon(float h) { return asin(-sqrt(h*(2*p.geometry.x+h))/(p.geometry.x+h)); }
#endif
