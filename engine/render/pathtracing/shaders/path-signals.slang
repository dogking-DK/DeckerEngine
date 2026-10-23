#ifndef DK_PATH_SIGNALS
#define DK_PATH_SIGNALS 0
#endif
#if DK_PATH_SIGNALS
[[vk::binding(15,0)]] RWStructuredBuffer<float4> signalOutput;
[[vk::binding(16,0)]] StructuredBuffer<float4> signalHistory;
static const uint signalStride=21;
struct SignalSample {
    float3 diffuse,specular,emission,background;
    float4 normalRoughness,positionDepth,albedoMetallic,motionDistance;
    uint triangle,flags;
};
SignalSample emptySignal() { SignalSample s={}; s.triangle=0xffffffffu; return s; }
struct SignalAccum { float4 data[21]; bool invalid; };
SignalAccum beginSignals(uint pixel) {
    SignalAccum a={}; uint first=asuint(parameters[0]).z;
    if(first!=0) {
        for(uint i=0;i<21;++i) a.data[i]=signalHistory[pixel*signalStride+i];
        if(asuint(a.data[6].w)!=first) a.invalid=true;
    }
    if(first==asuint(parameters[42]).x) {
        for(uint i=0;i<6;++i) { a.data[i]=0; a.data[13+i]=0; }
        for(uint i=8;i<13;++i) a.data[i]=0;
    }
    return a;
}
void signalSum(inout SignalAccum a,uint slot,uint correction,float3 value) {
    float3 sum=a.data[slot].xyz,c=a.data[correction].xyz;
    accumulate(sum,c,value); a.data[slot].xyz=sum; a.data[correction].xyz=c;
}
void addSignals(inout SignalAccum a,float3 radiance,SignalSample s,uint sample,uint randomSample,uint2 coordinate,float2 subpixel) {
    signalSum(a,0,13,radiance); signalSum(a,1,14,radiance*radiance);
    signalSum(a,2,15,s.diffuse); signalSum(a,3,16,s.specular);
    signalSum(a,4,17,s.emission); signalSum(a,5,18,s.background);
    signalSum(a,6,19,s.albedoMetallic.xyz); signalSum(a,7,20,s.normalRoughness.xyz);
    a.data[6].w=asfloat(asuint(a.data[6].w)+1); a.data[7].w=asfloat(asuint(a.data[7].w)+uint((s.flags&1)!=0));
    if(sample==asuint(parameters[42]).x) {
        if((s.flags&1)!=0 && asuint(parameters[42]).w!=0) {
            float4 p=float4(s.positionDepth.xyz,1);
            float4 clip=float4(dot(parameters[44],p),dot(parameters[45],p),dot(parameters[46],p),dot(parameters[47],p));
            if(all(isfinite(clip)) && clip.w>0 && clip.z>=0 && clip.z<=clip.w && all(abs(clip.xy)<=clip.w)) {
                float2 previous=(clip.xy/clip.w+1)*.5*float2(asuint(parameters[48]).xy);
                s.motionDistance.xy=previous-(float2(coordinate)+subpixel); s.flags|=2;
            }
        }
        a.data[8]=s.normalRoughness; a.data[9]=s.positionDepth; a.data[10]=s.albedoMetallic; a.data[11]=s.motionDistance;
        a.data[12]=asfloat(uint4(s.triangle,randomSample,s.flags,0));
    }
    a.data[12].w=asfloat(sample-asuint(parameters[42]).x+1);
}
void finishSignals(inout SignalAccum a,uint pixel) {
    uint4 identity=asuint(a.data[12]);
    if((identity.z&1)!=0 && (a.data[9].w<=0 || a.data[11].w<0 || abs(dot(a.data[8].xyz,a.data[8].xyz)-1)>1e-4 ||
        a.data[8].w<0 || a.data[8].w>1 || a.data[10].w<0 || a.data[10].w>1)) a.invalid=true;
    if(identity.w!=asuint(parameters[0]).z+asuint(parameters[0]).w-asuint(parameters[42]).x ||
        asuint(a.data[6].w)!=asuint(parameters[0]).z+asuint(parameters[0]).w || asuint(a.data[7].w)>asuint(a.data[6].w)) a.invalid=true;
    for(uint i=0;i<21;++i) {
        if(i!=12 && (!all(isfinite(a.data[i].xyz)) || (i!=6 && i!=7 && !isfinite(a.data[i].w)))) a.invalid=true;
        signalOutput[pixel*signalStride+i]=a.data[i];
    }
    for(uint i=0;i<7;++i) if(any(a.data[i].xyz<0)) a.invalid=true;
    if(a.invalid) { uint ignored; InterlockedOr(status[0],1,ignored); }
}
#endif
