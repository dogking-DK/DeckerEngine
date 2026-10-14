[[vk::binding(0,0)]] RWStructuredBuffer<float> values;
[numthreads(1,1,1)]
void computeMain(uint3 id : SV_DispatchThreadID) {
    float sum=0,correction=0;
    for(uint i=0;i<65536;++i) {
        float adjusted=values[0]-correction;
        float next=sum+adjusted;
        correction=(next-sum)-adjusted; sum=next;
    }
    values[1]=sum/65536;
}
