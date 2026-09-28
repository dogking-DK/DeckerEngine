struct Params { RWStructuredBuffer<uint> values; };
ParameterBlock<Params> params;
[shader("compute")]
[numthreads(1, 1, 1)]
void computeMain(uint3 index : SV_DispatchThreadID) { params.values[index.x] = 1; }
