struct Parameters { uint count; uint multiplier; uint addend; uint reserved; };
[[vk::push_constant]] ConstantBuffer<Parameters> parameters;
[[vk::binding(0, 0)]] StructuredBuffer<uint> inputValues;
[[vk::binding(1, 0)]] RWStructuredBuffer<uint> outputValues;

[shader("compute")]
[numthreads(64, 1, 1)]
void computeMain(uint3 index : SV_DispatchThreadID)
{
    if (index.x < parameters.count)
        outputValues[index.x] = inputValues[index.x] * parameters.multiplier + parameters.addend;
}
