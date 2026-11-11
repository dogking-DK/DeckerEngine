#ifndef DK_VOXEL_HDDA
#define DK_VOXEL_HDDA
#include "pnano-adapter.slang"
struct DkVoxelRay { float4 origin; float4 direction; int4 coordinate; };
struct DkVoxelResult { uint4 meta; int4 cell; float4 normalT; uint4 metrics; };
struct DkVoxelParameters { int4 lower; int4 upper; uint4 control; float4 normalRows[3]; };
bool dkContains(int3 p, DkVoxelParameters params) { return all(p >= params.lower.xyz) && all(p <= params.upper.xyz); }
int3 dkCells(float3 p, float3 direction, bool after) {
    float3 f = floor(p); int3 cell = int3(f);
    for (uint axis = 0; axis < 3; ++axis)
        if (p[axis] == f[axis] && (after ? direction[axis] < 0 : direction[axis] > 0)) --cell[axis];
    return cell;
}
uint dkValue(inout DkVoxelBuffer buf, inout pnanovdb_readaccessor_t acc, int3 cell, DkVoxelParameters params) {
    if (!dkContains(cell, params)) return 0;
    pnanovdb_address_t address=pnanovdb_readaccessor_get_value_address(PNANOVDB_GRID_TYPE_INT32,buf,acc,cell);
    return uint(pnanovdb_read_int32(buf,address));
}
bool dkTransition(inout DkVoxelBuffer buf, inout pnanovdb_readaccessor_t acc, float3 point, float time,
    float3 direction, DkVoxelParameters params, inout DkVoxelResult result) {
    int3 before = dkCells(point, direction, false), after = dkCells(point, direction, true);
    uint a = dkValue(buf, acc, before, params), b = dkValue(buf, acc, after, params);
    if ((a == 0) == (b == 0)) return false;
    uint axis = 0;
    while (axis < 3 && before[axis] == after[axis]) ++axis;
    if (axis == 3) return false;
    bool leaving = a != 0;
    bool positive = leaving ? direction[axis] > 0 : direction[axis] < 0;
    float3 n = float3(0); n[axis] = positive ? 1 : -1;
    float3 world = float3(dot(params.normalRows[0].xyz,n), dot(params.normalRows[1].xyz,n), dot(params.normalRows[2].xyz,n));
    world /= max(max(abs(world.x),abs(world.y)),abs(world.z));
    result.meta = uint4(1, leaving ? a : b, axis * 2 + (positive ? 1 : 0), 0);
    result.cell = int4(leaving ? before : after,0);
    result.normalT = float4(normalize(world),time);
    return true;
}
// Entity/asset transforms are composed once on the host. No camera far plane is imposed here.
DkVoxelResult dkTraceVoxel(inout DkVoxelBuffer buf, inout pnanovdb_readaccessor_t acc, DkVoxelRay ray, DkVoxelParameters params) {
    DkVoxelResult result = (DkVoxelResult)0;
    float3 origin = ray.origin.xyz, direction = ray.direction.xyz;
    float enter = -3.402823466e38f, leave = 3.402823466e38f;
    float3 entries, exits;
    for (uint axis = 0; axis < 3; ++axis) {
        float lo = float(params.lower[axis]), hi = float(params.upper[axis]) + 1;
        if (direction[axis] == 0) {
            if (origin[axis] <= lo || origin[axis] >= hi) return result;
            entries[axis] = -3.402823466e38f; exits[axis] = 3.402823466e38f;
        } else {
            float a = (lo-origin[axis])/direction[axis], b = (hi-origin[axis])/direction[axis];
            if (!isfinite(a) || !isfinite(b)) { result.meta.x = 4; return result; }
            entries[axis] = min(a,b); exits[axis] = max(a,b);
        }
        enter = max(enter, entries[axis]); leave = min(leave, exits[axis]);
    }
    if (enter >= leave || leave < ray.origin.w || enter > ray.direction.w) return result;
    float time = max(enter, ray.origin.w), end = min(leave, ray.direction.w);
    float3 point = mad(direction,float3(time),origin);
    for (uint axis=0; axis<3; ++axis) {
        if (time == entries[axis]) point[axis] = direction[axis] > 0 ? float(params.lower[axis]) : float(params.upper[axis])+1;
        if (time == exits[axis]) point[axis] = direction[axis] > 0 ? float(params.upper[axis])+1 : float(params.lower[axis]);
    }
    if (dkTransition(buf,acc,point,time,direction,params,result)) return result;
    int3 cell = dkCells(point,direction,true);
    while (dkContains(cell,params) && time < end) {
        if (buf.fault) { result.meta.x=6; return result; }
        if (result.metrics.x >= params.control.z) { result.meta.x=3; return result; }
        ++result.metrics.x;
        int dim = int(pnanovdb_readaccessor_get_dim(PNANOVDB_GRID_TYPE_INT32,buf,acc,cell));
        if (dim != 1 && dim != 8 && dim != 128 && dim != 4096) { result.meta.x=5; return result; }
        result.metrics.y += dim > 1 ? 1 : 0;
        float3 next, plane;
        for (uint axis=0; axis<3; ++axis) {
            // All hierarchy dimensions are powers of two. Masking is exact for
            // signed coordinates and directly expresses floor alignment.
            int block = cell[axis] & ~(dim-1);
            int low = max(block,params.lower[axis]), high = min(block+dim,params.upper[axis]+1);
            plane[axis] = float(direction[axis] > 0 ? high : low);
            next[axis] = direction[axis] == 0 ? 3.402823466e38f : (plane[axis]-origin[axis])/direction[axis];
        }
        float nextTime = min(next.x,min(next.y,next.z));
        if (!isfinite(nextTime) || nextTime <= time) { result.meta.x=4; return result; }
        if (nextTime > end) return result;
        point = mad(direction,float3(nextTime),origin);
        for (uint axis=0; axis<3; ++axis) if (next[axis] == nextTime) point[axis]=plane[axis];
        if (dkTransition(buf,acc,point,nextTime,direction,params,result)) return result;
        cell=dkCells(point,direction,true); time=nextTime;
    }
    return result;
}
DkVoxelResult dkQueryVoxel(inout DkVoxelBuffer buf, DkVoxelRay ray, DkVoxelParameters params) {
    DkVoxelResult result = (DkVoxelResult)0;
    pnanovdb_grid_handle_t grid; grid.address=pnanovdb_address_null();
    uint2 size=pnanovdb_grid_get_grid_size(buf,grid);
    if (pnanovdb_grid_get_grid_type(buf,grid) != PNANOVDB_GRID_TYPE_INT32 || size.y != 0 || size.x != buf.byteSize ||
        (pnanovdb_grid_get_version(buf,grid) >> 21) != 32) { result.meta.x=buf.fault ? 6 : 5; return result; }
    pnanovdb_readaccessor_t acc;
    pnanovdb_tree_handle_t tree=pnanovdb_grid_get_tree(buf,grid);
    pnanovdb_root_handle_t root=pnanovdb_tree_get_root(buf,tree);
    pnanovdb_readaccessor_init(acc,root);
    if (params.control.w == 1) {
        int3 cell=ray.coordinate.xyz;
        result.meta=uint4(2,dkValue(buf,acc,cell,params),0,0); result.cell=int4(cell,0);
        result.metrics.z=pnanovdb_readaccessor_get_dim(PNANOVDB_GRID_TYPE_INT32,buf,acc,cell);
        result.metrics.w=dkContains(cell,params) && pnanovdb_readaccessor_is_active(PNANOVDB_GRID_TYPE_INT32,buf,acc,cell) ? 1 : 0;
    } else result=dkTraceVoxel(buf,acc,ray,params);
    if (buf.fault) result.meta.x=6;
    return result;
}
#endif
