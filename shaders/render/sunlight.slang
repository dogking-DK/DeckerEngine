// Lambert only. Local sky coordinates: Y = radial up, X = projected sun.
struct DrawData { float4 row0; float4 row1; float4 row2; float4 row3; float4 baseColor; float4 emissive; };
[[vk::push_constant]] ConstantBuffer<DrawData> drawData;
struct Lighting {
    float4 normal0; float4 normal1; float4 normal2; // normal0.w = authored normals present
    float4 world0; float4 world1; float4 world2; // Position relative to sky anchor, km
    float4 geometry; // R, H, anchor height, sky diffuse enabled
    float4 sun; // Local unit direction towards sun; w = determinant sign
    float4 energy; // RGB irradiance; w = double sided
    float4 shadowU,shadowV; // Anchor-relative km to fixed light-space UV.
};
[[vk::binding(2,0)]] ConstantBuffer<Lighting> light;
struct VertexOutput {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    float3 normal : TEXCOORD1;
    float3 relative : TEXCOORD2;
};
#if defined(DK_VERTEX)
[shader("vertex")]
VertexOutput vertexMain(float3 position : POSITION, float2 uv : TEXCOORD0, float3 normal : NORMAL)
{
    VertexOutput result; float4 p=float4(position,1);
    result.position=float4(dot(drawData.row0,p),dot(drawData.row1,p),dot(drawData.row2,p),dot(drawData.row3,p));
    result.uv=uv;
    normal/=max(max(abs(normal.x),max(abs(normal.y),abs(normal.z))),1e-30);
    normal*=rsqrt(max(dot(normal,normal),1e-30));
    result.normal=float3(dot(light.normal0.xyz,normal),dot(light.normal1.xyz,normal),dot(light.normal2.xyz,normal));
    result.relative=float3(dot(light.world0,p),dot(light.world1,p),dot(light.world2,p));
    return result;
}
#else
[[vk::binding(0,0)]] Texture2D<float4> baseTexture;
[[vk::binding(1,0)]] SamplerState baseSampler;
bool frontSide(bool front) { return light.sun.w<0 ? !front : front; }
float3 surface(VertexOutput input,bool front) {
    if(!frontSide(front) && light.energy.w==0) discard;
    float4 value=baseTexture.Sample(baseSampler,input.uv)*drawData.baseColor;
    if(value.a<drawData.emissive.w) discard;
    return value.rgb;
}
#if defined(DK_DEPTH)
[shader("fragment")]
void depthMain(VertexOutput input,bool front:SV_IsFrontFace) { surface(input,front); }
#else
[[vk::binding(3,0)]] Texture2D<float4> transmittanceLut;
[[vk::binding(4,0)]] Texture2D<float4> skyLut;
[[vk::binding(5,0)]] SamplerState linearClamp;
[[vk::binding(6,0)]] Texture2D<float> cloudShadow;
static const float PI=3.14159265358979323846;
float2 roots(float h,float mu,float boundary) {
    float b=(light.geometry.x+h)*mu,c=(h-boundary)*(2*light.geometry.x+h+boundary),d=b*b-c;
    if(d<0) return float2(1,-1);
    float q=-b-(b>=0 ? 1 : -1)*sqrt(d);
    if(q==0) return 0;
    return float2(min(q,c/q),max(q,c/q));
}
float3 sunlight(float h,float mu) {
    float2 ground=roots(h,mu,0);
    if(mu<0 && ground.x<=ground.y && ground.x>=0) return 0;
    float2 top=roots(h,mu,light.geometry.y);
    if(top.x>top.y || top.y<=0) return 1;
    if(h>light.geometry.y) {
        mu=clamp(((light.geometry.x+h)*mu+max(0,top.x))/(light.geometry.x+light.geometry.y),-1,1);
        h=light.geometry.y;
        top=roots(h,mu,light.geometry.y);
    }
    float H=sqrt(light.geometry.y*(2*light.geometry.x+light.geometry.y));
    float rho=sqrt(max(0,h*(2*light.geometry.x+h)));
    float dmin=light.geometry.y-h,dmax=rho+H;
    float2 uv=saturate(float2((top.y-dmin)/max(dmax-dmin,1e-8),rho/H));
    return transmittanceLut.SampleLevel(linearClamp,(uv*float2(255,63)+.5)/float2(256,64),0).rgb;
}
float3 sky(float3 d) {
    float R=light.geometry.x,h=light.geometry.z;
    float eh=asin(-sqrt(h*(2*R+h))/(R+h)),elevation=asin(clamp(d.y,-1,1)),row;
    if(elevation<eh) row=53*(1-sqrt(saturate((eh-elevation)/(eh+PI*.5))));
    else row=54+53*sqrt(saturate((elevation-eh)/(PI*.5-eh)));
    float phi=atan2(d.z,d.x);
    return skyLut.SampleLevel(linearClamp,float2(((phi/PI*.5+.5)*191+.5)/192,(row+.5)/108),0).rgb;
}
[shader("fragment")]
float4 fragmentMain(VertexOutput input,bool front:SV_IsFrontFace):SV_Target0 {
    float3 albedo=surface(input,front),n=input.normal;
    if(light.normal0.w==0 || dot(n,n)<1e-20) {
        // With a positive viewport height, -cross(dP/dx,dP/dy) faces the viewer.
        // Derivatives follow screen axes, independent of index winding or mirroring.
        // Do not apply the authored-normal back-face flip to this fallback.
        n=-cross(ddx(input.relative),ddy(input.relative));
    }
    else if(!frontSide(front)) n=-n;
    n*=rsqrt(max(dot(n,n),1e-30));
    float r=light.geometry.x+light.geometry.z;
    float delta=dot(input.relative,input.relative)+2*r*input.relative.y;
    float h=max(0,light.geometry.z+delta/(sqrt(max(0,r*r+delta))+r));
    float mu=clamp((dot(input.relative,light.sun.xyz)+r*light.sun.y)/(light.geometry.x+h),-1,1);
    float2 shadowUV=float2(dot(light.shadowU,float4(input.relative,1)),dot(light.shadowV,float4(input.relative,1)));
    float3 irradiance=sunlight(h,mu)*max(0,dot(n,light.sun.xyz))*cloudShadow.SampleLevel(linearClamp,shadowUV,0);
    if(light.geometry.w>0) {
        float3 diffuse=0;
        [loop] for(uint y=0;y<16;++y) {
            float dy=1-2*(float(y)+.5)/16,s=sqrt(1-dy*dy);
            [loop] for(uint x=0;x<16;++x) {
                float phi=2*PI*(float(x)+.5)/16;
                float3 d=float3(s*cos(phi),dy,s*sin(phi));
                diffuse+=sky(d)*max(0,dot(n,d));
            }
        }
        irradiance+=diffuse*(4*PI/256);
    }
    return float4(albedo*(irradiance*light.energy.rgb/PI)+drawData.emissive.rgb,1);
}
#endif
#endif
