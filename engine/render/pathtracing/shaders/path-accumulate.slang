[[vk::binding(7,0)]] StructuredBuffer<float4> history;
[[vk::binding(8,0)]] RWStructuredBuffer<uint> status;
struct Accumulation {
    float3 sum, moment, correction, momentCorrection;
    uint rays;
    uint2 previousRays;
    float invalid;
    float4 hit, normal;
};
void accumulate(inout float3 sum,inout float3 correction,float3 value) {
    precise float3 adjusted=value-correction;
    precise float3 next=sum+adjusted;
    correction=(next-sum)-adjusted; sum=next;
}
Accumulation beginAccumulation(uint pixel) {
    Accumulation a={}; a.hit=float4(-1,0,0,0);
    uint4 image=asuint(parameters[0]);
    if(asuint(parameters[11]).w!=0 && image.z!=0) {
        uint at=pixel*8;
        a.sum=history[at+4].xyz; a.correction=history[at+5].xyz;
        a.moment=history[at+6].xyz; a.momentCorrection=history[at+7].xyz;
        a.previousRays=uint2(asuint(history[at+5].w),asuint(history[at+6].w)); a.invalid=history[at+1].w;
        a.hit=history[at+2]; a.normal=history[at+3];
        if(asuint(history[at+4].w)!=image.z) a.invalid=1;
    }
    return a;
}
void addSample(inout Accumulation a,float3 value,uint sample,float4 hit,float4 normal) {
    if(!all(isfinite(value)) || any(value<0)) a.invalid=1;
    accumulate(a.sum,a.correction,value); accumulate(a.moment,a.momentCorrection,value*value);
    if(sample==0 || (asuint(parameters[11]).w==0 && sample==asuint(parameters[0]).z)) { a.hit=hit; a.normal=normal; }
}
void finishAccumulation(Accumulation a,uint pixel,uint localPixel) {
    uint4 image=asuint(parameters[0]); bool progressive=asuint(parameters[11]).w!=0;
    uint count=progressive ? image.z+image.w : image.w;
    uint at=progressive ? pixel*8 : localPixel*4;
    if(!all(isfinite(a.sum)) || !all(isfinite(a.moment)) || !all(isfinite(a.correction)) || !all(isfinite(a.momentCorrection))) a.invalid=1;
    output[at]=float4(a.sum/count,float(a.rays)); output[at+1]=float4(a.moment/count,a.invalid);
    output[at+2]=a.hit; output[at+3]=a.normal;
    if(progressive) {
        uint low=a.previousRays.x+a.rays;
        uint high=a.previousRays.y+uint(low<a.previousRays.x);
        output[at+4]=float4(a.sum,asfloat(count)); output[at+5]=float4(a.correction,asfloat(low));
        output[at+6]=float4(a.moment,asfloat(high)); output[at+7]=float4(a.momentCorrection,0);
    }
    if(a.invalid!=0) { uint ignored; InterlockedOr(status[0],1,ignored); }
}
