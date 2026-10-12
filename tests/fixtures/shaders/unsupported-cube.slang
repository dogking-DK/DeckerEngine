TextureCube<float4> cube;
SamplerState sampleState;
RWStructuredBuffer<float4> result;
[shader("compute")]
[numthreads(1, 1, 1)]
void computeMain() { result[0] = cube.SampleLevel(sampleState, float3(1, 0, 0), 0); }
