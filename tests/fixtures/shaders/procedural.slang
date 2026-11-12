#include "raytracing.slang"
#ifdef DK_NANOVDB
#include "pnano-adapter.slang"
[[vk::binding(3,0)]] StructuredBuffer<uint> voxelWords;
int sampleValue(uint primitive) {
    uint count,stride; voxelWords.GetDimensions(count,stride);
    DkVoxelBuffer buf={voxelWords,count*4,stride!=4};
    pnanovdb_grid_handle_t grid; grid.address=pnanovdb_address_null();
    uint2 size=pnanovdb_grid_get_grid_size(buf,grid);
    if (size.y || size.x!=buf.byteSize || pnanovdb_grid_get_grid_type(buf,grid)!=PNANOVDB_GRID_TYPE_INT32 ||
        (pnanovdb_grid_get_version(buf,grid)>>21)!=32) return 0;
    pnanovdb_readaccessor_t acc;
    pnanovdb_readaccessor_init(acc,pnanovdb_tree_get_root(buf,pnanovdb_grid_get_tree(buf,grid)));
    int3 coord=primitive==0 ? int3(7,8,9) : int3(-128,-128,-128);
    pnanovdb_address_t address=pnanovdb_readaccessor_get_value_address(PNANOVDB_GRID_TYPE_INT32,buf,acc,coord);
    int value=pnanovdb_read_int32(buf,address);
    return buf.fault ? 0 : value;
}
#else
int sampleValue(uint primitive) { return primitive==0 ? 37 : 53; }
#endif
struct SurfaceAttributes {
#ifdef DK_BAD_ATTRIBUTE
    uint value;
#else
    int value;
#endif
    float marker;
};
[shader("intersection")]
void intersectionMain() {
    float3 center=float3(4*GeometryIndex(),3*PrimitiveIndex(),0);
    float3 origin=ObjectRayOrigin()-center, direction=ObjectRayDirection();
    float a=dot(direction,direction), b=dot(origin,direction), c=dot(origin,origin)-1;
    float d=b*b-a*c;
    if (d<0) return;
    int value=sampleValue(PrimitiveIndex());
    if (!value) return;
    SurfaceAttributes attrs; attrs.value=value; attrs.marker=19;
    float first=(-b-sqrt(d))/a, last=(-b+sqrt(d))/a;
    if (first>=RayTMin() && first<=RayTCurrent()) ReportHit(first,0,attrs);
    if (last>=RayTMin() && last<=RayTCurrent()) ReportHit(last,0,attrs);
}
[shader("closesthit")]
void proceduralHit(inout HitPayload payload, SurfaceAttributes attributes) {
    payload.distance=RayTCurrent(); payload.barycentric=float2(float(attributes.value),attributes.marker);
    payload.instance=InstanceID(); payload.primitive=PrimitiveIndex(); payload.geometry=GeometryIndex(); payload.hit=4;
}
[shader("anyhit")]
void proceduralAny(inout HitPayload payload, SurfaceAttributes attributes) {
    if (InstanceID()==9 || attributes.value==0) IgnoreHit();
}
