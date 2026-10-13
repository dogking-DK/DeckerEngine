// Shared metre-space cloud density, geometry and lighting. Uses cp and immutable fields.
static const float CLOUD_PI=3.14159265358979323846;
float2 cloudRoots(float h,float mu,float boundary) {
    float b=(cp.geometry.x+h)*mu,c=(h-boundary)*(2*cp.geometry.x+h+boundary),d=b*b-c;
    if(d<=0) return float2(1,-1);
    float q=-b-(b>=0 ? 1 : -1)*sqrt(d),other=c/q;
    return float2(min(q,other),max(q,other));
}
float cloudHeightAt(float h,float mu,float t) {
    float r=cp.geometry.x+h,delta=t*(t+2*r*mu);
    return h+delta/(sqrt(max(0,r*r+delta))+r);
}
float cloudSmooth01(float x) { x=saturate(x); return x*x*(3-2*x); }
float cloudDensity(CloudRayData ray,float3 displacement,float height) {
    if(cp.flags.w!=0) return cp.cloud.y*cp.cloud.w;
    if(height<=cp.geometry.z || height>=cp.geometry.w) return 0;
    float2 weather=weatherField.SampleLevel(linearRepeat,ray.weather.xy+displacement.xz/cp.scales.x,0);
    float base=baseField.SampleLevel(linearRepeat,ray.base.xyz+displacement/cp.scales.y,0);
    float detail=detailField.SampleLevel(linearRepeat,ray.detail.xyz+displacement/cp.scales.z,0);
    float coverage=saturate((weather.x-(1-cp.cloud.x))/cp.cloud.x);
    float h=(height-cp.geometry.z)/(cp.geometry.w-cp.geometry.z);
    float profile=cloudSmooth01(h/cp.scales.w)*cloudSmooth01((.6+.4*weather.y-h)/cp.lighting.w);
    return cp.cloud.y*saturate((base*profile-(1-coverage))/.35-.2*detail);
}
// Up to two cloudShell segments. Ground occultation is independent of the cloud cloudShell.
uint cloudShell(float h,float mu,out float4 segments,out bool ground) {
    segments=0; ground=false; float limit=2*(cp.geometry.x+cp.geometry.w);
    float2 hit=cloudRoots(h,mu,0);
    if(mu<0 && hit.x>=0 && hit.x<=hit.y && hit.x<=limit) { limit=hit.x; ground=true; }
    float2 outer=cloudRoots(h,mu,cp.geometry.w);
    float begin=max(0,outer.x),end=min(limit,outer.y);
    if(end<=begin) return 0;
    float2 inner=cloudRoots(h,mu,cp.geometry.z);
    float a=max(begin,inner.x),b=min(end,inner.y);
    if(b<=a) { segments.xy=float2(begin,end); return 1; }
    uint count=0;
    if(a>begin) { segments.xy=float2(begin,a); count=1; }
    if(end>b) { if(count==0) segments.xy=float2(b,end); else segments.zw=float2(b,end); ++count; }
    return count;
}
float cloudSunlightOptical(CloudRayData ray,float3 position,float h,float mu,uint minimumSteps=0) {
    uint steps=max(cp.counts.y,minimumSteps);
    float4 segments; bool ground; uint count=cloudShell(h,mu,segments,ground);
    if(ground) return -1;
    if(cp.flags.z==0) return 0;
    float optical=0;
    [loop] for(uint k=0;k<count;++k) {
        float a=segments[2*k],b=segments[2*k+1],ds=(b-a)/float(steps);
        [loop] for(uint j=0;j<steps;++j) {
            float t=a+(float(j)+.5)*ds;
            optical+=cloudDensity(ray,position+cp.sun.xyz*t,cloudHeightAt(h,mu,t))*cp.cloud.z*ds;
        }
    }
    return optical;
}
float cloudNode(uint i,uint count) { return .5-.5*cos(CLOUD_PI*float(i)/float(count)); }
float3 cloudAirTransmittance(float h,float mu) {
    if(cp.flags.y==0) return 1;
    float2 top=cloudRoots(h,mu,cp.geometry.y);
    if(top.x>top.y || top.y<0) return 1;
    float begin=max(0,top.x),end=top.y;
    float3 optical=0;
    [loop] for(uint i=0;i<cp.counts.z;++i) {
        float a=lerp(begin,end,cloudNode(i,cp.counts.z)),b=lerp(begin,end,cloudNode(i+1,cp.counts.z));
        float height=max(0,cloudHeightAt(h,mu,(a+b)*.5));
        float3 extinction=cp.rayleigh.xyz*exp(-height/cp.rayleigh.w)+cp.mie.xyz*exp(-height/cp.mie.w)
            +cp.absorption.xyz*max(0,1-abs(height-cp.absorption.w)/cp.air.x);
        optical+=extinction*(b-a);
    }
    return exp(-optical);
}
float cloudHG(float cosine,float g) { return (1-g*g)/(4*CLOUD_PI*pow(1+g*g-2*g*cosine,1.5)); }
float cloudAbsorbed(float optical) {
    return optical<.001 ? optical*(1-optical*.5+optical*optical/6) : 1-exp(-optical);
}
