// Model v3: Lambert or conservative metallic-roughness, geometric normal only.
struct PathBsdf { float3 color; float metallic; float roughness; bool ggx; };
struct BsdfEval { float3 value; float pdf; };
float fifth(float x) { float x2=x*x; return x2*x2*x; }
BsdfEval evalBsdf(PathBsdf b,float3 n,float3 wo,float3 wi) {
    float no=dot(n,wo),ni=dot(n,wi); BsdfEval result={};
    if(no<=0 || ni<=0) return result;
    if(!b.ggx) { result.value=b.color/pi; result.pdf=ni/pi; return result; }
    float3 h=normalize(wo+wi); float nh=saturate(dot(n,h)),vh=saturate(dot(wo,h));
    float a=max(.0025,b.roughness*b.roughness),a2=a*a;
    float3 crossed=cross(n,h); float denominator=dot(crossed,crossed)+a2*nh*nh;
    float d=a2/(pi*denominator*denominator),so=sqrt(a2+(1-a2)*no*no),si=sqrt(a2+(1-a2)*ni*ni);
    float3 f0=lerp(float3(.04),b.color,b.metallic),f=f0+(1-f0)*fifth(1-vh);
    float transmission=.96*.96*(1-fifth(1-no))*(1-fifth(1-ni)),q=(1+b.metallic)*.5;
    result.value=b.color*((1-b.metallic)*transmission/pi)+f*(d/(2*(ni*so+no*si)));
    result.pdf=(1-q)*ni/pi+q*d/(2*(no+so)); return result;
}
float3 sampleBsdf(PathBsdf b,float3 n,float3 wo,float u,float v) {
    float3 axis=abs(n.z)<.9 ? float3(0,0,1) : float3(0,1,0);
    float3 x=normalize(cross(axis,n)),y=cross(n,x);
    float q=b.ggx ? (1+b.metallic)*.5 : 0,phi=2*pi*v;
    if(u>=q) {
        float r=sqrt((u-q)/(1-q)); return normalize(x*(r*cos(phi))+y*(r*sin(phi))+n*sqrt(max(0.0,1-r*r)));
    }
    float a=max(.0025,b.roughness*b.roughness);
    float3 o=float3(dot(x,wo),dot(y,wo),dot(n,wo)),stretched=normalize(float3(a*o.xy,o.z));
    float lensq=dot(stretched.xy,stretched.xy);
    float3 t1=lensq>0 ? float3(-stretched.y,stretched.x,0)/sqrt(lensq) : float3(1,0,0),t2=cross(stretched,t1);
    float r=sqrt(u/q),px=r*cos(phi),s=(1+stretched.z)*.5;
    float py=(1-s)*sqrt(max(0.0,1-px*px))+s*r*sin(phi);
    float3 projected=t1*px+t2*py+stretched*sqrt(max(0.0,1-px*px-py*py));
    float3 local=float3(a*projected.xy,max(0.0,projected.z));
    float3 h=normalize(x*local.x+y*local.y+n*local.z);
    return normalize(2*dot(wo,h)*h-wo); // Below-surface results carry zero evaluated mass.
}
