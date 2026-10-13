struct CloudParameters {
    float4 geometry; // R, atmosphere H, cloud bottom, cloud top
    float4 cloud; // coverage, cloudDensity, extinction, homogeneous cloudDensity
    float4 scales; // weather, base, detail, bottom fade
    float4 lighting; // g, albedo, multiple-scattering redistribution, top fade
    float4 sun, irradiance, sky;
    float4 rayleigh, mie, absorption, air;
    uint4 counts; // primary, sunlight, atmosphere, ray count
    uint4 flags; // clouds enabled, atmosphere enabled, self shadow, homogeneous
};
struct CloudRayData { float4 geometry,direction,weather,base,detail,segments; };
struct CloudIntegralData { float4 color,statistics; };
