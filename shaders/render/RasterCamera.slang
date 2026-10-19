// Kilometre-space rays relative to the eye's radial basis. Orthographic media
// start at each pixel's near plane; depth is distance after that plane.
float3 cameraRay(float2 screen) {
    return p.origin.w!=0 ? p.forward.xyz : p.forward.xyz+screen.x*p.right.xyz+screen.y*p.up.xyz;
}
float3 cameraOrigin(float2 screen) {
    return p.origin.w!=0 ? p.origin.xyz+screen.x*p.right.xyz+screen.y*p.up.xyz : float3(0);
}
float cameraDistance(float depth,float3 ray) {
    return p.origin.w!=0 ? depth*p.depth.z : p.depth.y/(depth+p.depth.x)*length(ray);
}
struct CameraMediumRay { float3 direction; float height,mu,sunMu,cosine; };
CameraMediumRay mediumRay(float2 screen) {
    CameraMediumRay ray; ray.direction=normalize(cameraRay(screen));
    float3 offset=cameraOrigin(screen); float r=p.geometry.x+p.geometry.z;
    float delta=dot(offset,offset)+2*r*offset.y;
    ray.height=p.geometry.z+delta/(sqrt(max(0,r*r+delta))+r);
    float3 radial=(float3(0,r,0)+offset)/(p.geometry.x+ray.height);
    ray.mu=clamp(dot(radial,ray.direction),-1,1);
    ray.sunMu=clamp(dot(radial,p.sunDirection.xyz),-1,1);
    ray.cosine=clamp(dot(ray.direction,p.sunDirection.xyz),-1,1);
    return ray;
}
