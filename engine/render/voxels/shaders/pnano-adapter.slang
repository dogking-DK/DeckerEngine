// PNanoVDB 32.7.0 HLSL, little-endian Int32, 32-bit byte addresses.
// Propagate a per-invocation fault through every accessor call; OOB never becomes a miss.
#ifndef DK_PNANO_ADAPTER
#define DK_PNANO_ADAPTER
#define PNANOVDB_HLSL
#define PNANOVDB_BUF_CUSTOM
struct DkVoxelBuffer {
    StructuredBuffer<uint> words;
    uint byteSize;
    bool fault;
#ifdef DK_PNANO_SUBRANGE
    uint baseWord;
#endif
};
#define pnanovdb_buf_t inout DkVoxelBuffer
#define pnanovdb_grid_type_t uint
#define PNANOVDB_GRID_TYPE_GET(type, name) pnanovdb_grid_type_constants[type].name
uint pnanovdb_buf_read_uint32(inout DkVoxelBuffer buf, uint offset) {
    if ((offset & 3u) != 0 || buf.byteSize < 4 || offset > buf.byteSize - 4) {
        buf.fault = true; return 0;
    }
#ifdef DK_PNANO_SUBRANGE
    return buf.words[buf.baseWord+(offset >> 2u)];
#else
    return buf.words[offset >> 2u];
#endif
}
uint2 pnanovdb_buf_read_uint64(inout DkVoxelBuffer buf, uint offset) {
    if ((offset & 3u) != 0 || buf.byteSize < 8 || offset > buf.byteSize - 8) {
        buf.fault = true; return uint2(0);
    }
    uint low=pnanovdb_buf_read_uint32(buf,offset);
    uint high=pnanovdb_buf_read_uint32(buf,offset+4);
    return uint2(low,high);
}
void pnanovdb_buf_write_uint32(inout DkVoxelBuffer buf, uint offset, uint value) { buf.fault = true; }
void pnanovdb_buf_write_uint64(inout DkVoxelBuffer buf, uint offset, uint2 value) { buf.fault = true; }
#include <nanovdb/PNanoVDB.h>
#ifndef PNANOVDB_ADDRESS_32
#error DeckerEngine requires PNanoVDB 32-bit addresses
#endif
#endif
