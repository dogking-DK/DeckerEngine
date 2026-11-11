#include "pnano-adapter.slang"
[[vk::binding(0,0)]] StructuredBuffer<uint> words;
[[vk::binding(1,0)]] RWStructuredBuffer<uint2> result;
[shader("compute")]
[numthreads(8,1,1)]
void computeMain(uint3 tid : SV_DispatchThreadID) {
    DkVoxelBuffer buf={words,64,false}; uint v=0;
    switch (tid.x) {
        case 0: v=pnanovdb_buf_read_uint32(buf,2); break;
        case 1: v=pnanovdb_buf_read_uint32(buf,64); break;
        case 2: v=pnanovdb_buf_read_uint32(buf,0xffffffff); break;
        case 3: v=pnanovdb_buf_read_uint64(buf,60).x; break;
        case 4: v=pnanovdb_buf_read_uint64(buf,0xfffffffc).x; break;
        case 5: v=pnanovdb_buf_read_uint32(buf,60); break;
        case 6: v=pnanovdb_buf_read_uint32(buf,0); break;
        case 7: pnanovdb_buf_write_uint32(buf,0,1); break;
    }
    result[tid.x]=uint2(v,buf.fault ? 1 : 0);
}
