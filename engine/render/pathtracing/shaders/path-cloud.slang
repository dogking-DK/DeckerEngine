// Immutable packed RG8 weather / R8 volume fields; normalized repeat, texel centres.
[[vk::binding(9,0)]] StructuredBuffer<uint> cloudFields;
float cloudByte(uint at) { return float((cloudFields[at/4]>>((at%4)*8))&255u)/255.0; }
float cloudField(float3 uv,uint size,uint offset,uint channels,uint channel,bool volume) {
    float3 p=frac(uv)*float(size)-.5,blend=frac(p); int3 lo=int3(floor(p)); float value=0;
    [unroll] for(uint z=0;z<2;++z) {
        if(!volume && z!=0) continue;
        [unroll] for(uint y=0;y<2;++y) [unroll] for(uint x=0;x<2;++x) {
            uint3 at=uint3(lo+int3(x,y,z))&(size-1);
            uint index=at.x+size*(at.y+(volume ? size*at.z : 0));
            float w=(x!=0 ? blend.x : 1-blend.x)*(y!=0 ? blend.y : 1-blend.y)*(volume ? (z!=0 ? blend.z : 1-blend.z) : 1);
            value+=cloudByte(offset+index*channels+channel)*w;
        }
    }
    return value;
}
float cloudSmooth(float x) { x=saturate(x); return x*x*(3-2*x); }
float cloudDensity(AirLocation a) {
    uint4 sizes=asuint(parameters[22]); float4 shell=parameters[23];
    if(sizes.y==0 || a.height<=shell.x || a.height>=shell.y) return 0;
    float2 weather=float2(cloudField(float3(a.weather,0),sizes.y,0,2,0,false),cloudField(float3(a.weather,0),sizes.y,0,2,1,false));
    uint baseOffset=sizes.y*sizes.y*2,detailOffset=baseOffset+sizes.z*sizes.z*sizes.z;
    float base=cloudField(a.base,sizes.z,baseOffset,1,0,true),detail=cloudField(a.detail,sizes.w,detailOffset,1,0,true);
    float coverage=saturate((weather.x-(1-shell.z))/shell.z),h=(a.height-shell.x)/(shell.y-shell.x);
    float profile=cloudSmooth(h/parameters[25].w)*cloudSmooth((.6+.4*weather.y-h)/parameters[24].w);
    return shell.w*saturate((base*profile-(1-coverage))/.35-.2*detail);
}
float cloudShift(float phase,float delta,uint scale) {
    float high=parameters[25][scale],low=parameters[29][scale],product=delta*high;
    // Keep the fractional part even when a long displacement spans many periods.
    return frac(frac(product)+(fma(delta,high,-product)+delta*low)+phase);
}
