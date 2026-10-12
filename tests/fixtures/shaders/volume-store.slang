#ifndef VOLUME_FORMAT
#define VOLUME_FORMAT "rgba16f"
#endif
[[vk::binding(0, 0)]] [[vk::image_format(VOLUME_FORMAT)]] RWTexture3D<float4> volume;
[shader("compute")]
[numthreads(1, 1, 1)]
void computeMain(uint3 id : SV_DispatchThreadID) { volume[id] = float4(float3(id), 1); }
