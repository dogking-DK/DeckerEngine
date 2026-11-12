#pragma once
#include <dk/math/Types.hpp>
#include <cmath>

namespace dk {
// Session-only raster values, shared by Editor and independent capture.
struct VoxelDisplaySettings {
    bool directional=false;
    Vec3d light_direction{0,0,1};
    Vec3d irradiance=Vec3d::Ones();
    bool operator==(const VoxelDisplaySettings&) const = default;
    [[nodiscard]] bool valid() const noexcept {
        return light_direction.allFinite() && std::abs(light_direction.norm()-1)<=1e-6 &&
            irradiance.allFinite() && (irradiance.array()>=0).all() && (irradiance.array()<=1e6).all();
    }
};
}
