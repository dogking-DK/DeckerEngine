#pragma once
#include <dk/render/CloudReference.hpp>
#include <algorithm>
#include <cmath>

namespace cloud_cases {
using namespace dk;
using namespace dk::render;
inline EnvironmentDescription environment() {
    EnvironmentDescription e; e.enabled=true; e.clouds.enabled=true; e.clouds.coverage=.85;
    e.sun.direction={.6,.8,0}; e.sun.irradiance={1,.9,.8}; return e;
}
inline std::array<CloudRay,4> rays() {
    return {CloudRay{{0,100,0},{0,1,0},20000},CloudRay{{1300,2200,-700},{.7,.25,.1},20000},
        CloudRay{{-2000,6000,900},{.2,-1,0},20000},CloudRay{{0,2200,0},{1,0,0},40000}};
}
inline CloudReferenceSettings settings(unsigned level=0) {
    CloudReferenceSettings s; s.steps=256u<<level; s.sun_steps=s.atmosphere_steps=64u<<level;
    s.time_seconds=17.25; s.sky_radiance={.015,.02,.03}; return s;
}
inline double rgb_error(const CloudIntegral& a,const CloudIntegral& b) {
    return (a.radiance-b.radiance).cwiseAbs().maxCoeff()/std::max(.01,b.radiance.maxCoeff());
}
}
