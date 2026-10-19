// World-space origin displacement is essential for texture phase and history.
float3 cloudCameraOrigin(float2 screen) {
    return p.origin.w!=0 ? cloudFrame.forward.xyz*cloudFrame.forward.w+
        screen.x*cloudFrame.right.xyz+screen.y*cloudFrame.up.xyz : float3(0);
}
float3 cloudCameraDirection(float2 screen) {
    return normalize(p.origin.w!=0 ? cloudFrame.forward.xyz :
        cloudFrame.forward.xyz+screen.x*cloudFrame.right.xyz+screen.y*cloudFrame.up.xyz);
}
CloudRayData cloudCameraRay(float2 screen,CameraMediumRay air) {
    CloudRayData ray=cloudFrame.origin; float3 offset=cloudCameraOrigin(screen);
    ray.direction.xyz=cloudCameraDirection(screen);
    ray.geometry=float4(air.height*1000,air.mu,air.sunMu,air.cosine);
    ray.weather.xy+=offset.xz/cp.scales.x;
    ray.base.xyz+=offset/cp.scales.y; ray.detail.xyz+=offset/cp.scales.z;
    return ray;
}
