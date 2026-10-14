struct HitPayload {
#ifdef DK_BAD_PAYLOAD
    uint distance;
#else
    float distance;
#endif
    float2 barycentric;
    uint instance;
    uint primitive;
    uint geometry;
    uint hit;
};
[[vk::binding(0, 0)]] RaytracingAccelerationStructure scene;
[[vk::binding(1, 0)]] StructuredBuffer<float4> inputRays;
[[vk::binding(2, 0)]] RWStructuredBuffer<float4> results;

[shader("raygeneration")]
void rayMain() {
    uint index = DispatchRaysIndex().x;
    float4 origin = inputRays[index*3];
    float4 direction = inputRays[index*3+1];
    RayDesc ray; ray.Origin=origin.xyz; ray.TMin=origin.w; ray.Direction=direction.xyz; ray.TMax=direction.w;
    uint mask=uint(inputRays[index*3+2].x);
    HitPayload hit = {};
#ifdef DK_DYNAMIC_SELECTOR
    TraceRay(scene, RAY_FLAG_NONE, mask, uint(inputRays[index*3+2].y), 2, 0, ray, hit);
#else
    TraceRay(scene, RAY_FLAG_NONE, mask, 0, 2, 0, ray, hit);
#endif
    results[index*2] = float4(hit.distance,hit.barycentric,float(hit.instance));
    HitPayload visibility = {}; visibility.hit=1;
    TraceRay(scene, RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH|RAY_FLAG_SKIP_CLOSEST_HIT_SHADER, mask, 1, 2, 1, ray, visibility);
    results[index*2+1] = float4(float(hit.primitive),float(hit.geometry),float(hit.hit),float(visibility.hit));
}
[shader("miss")]
void missMain(inout HitPayload payload) { payload.distance=-1; payload.instance=999; payload.hit=0; }
[shader("miss")]
void visibilityMiss(inout HitPayload payload) { payload.hit=0; }
[shader("closesthit")]
void hitMain(inout HitPayload payload, BuiltInTriangleIntersectionAttributes attributes) {
    payload.distance=RayTCurrent(); payload.barycentric=attributes.barycentrics;
    payload.instance=InstanceID(); payload.primitive=PrimitiveIndex(); payload.geometry=GeometryIndex(); payload.hit=1;
}
[shader("closesthit")]
void hitGeometry(inout HitPayload payload, BuiltInTriangleIntersectionAttributes attributes) {
    payload.distance=RayTCurrent(); payload.barycentric=attributes.barycentrics;
    payload.instance=InstanceID(); payload.primitive=PrimitiveIndex(); payload.geometry=GeometryIndex(); payload.hit=2;
}
[shader("closesthit")]
void hitAlternate(inout HitPayload payload, BuiltInTriangleIntersectionAttributes attributes) {
    payload.distance=RayTCurrent(); payload.barycentric=attributes.barycentrics;
    payload.instance=InstanceID(); payload.primitive=PrimitiveIndex(); payload.geometry=GeometryIndex(); payload.hit=3;
}
[shader("anyhit")]
void anyMain(inout HitPayload payload, BuiltInTriangleIntersectionAttributes attributes) {
    if (InstanceID()==9) IgnoreHit();
}
