#pragma once
#include <dk/render/CloudReference.hpp>
#include "CloudDensityInternal.hpp"

namespace dk::render::detail {
struct CloudPreparedRay {
    CloudRay ray;
    CloudIntervals intervals;
    double height=0, mu=0, sun_mu=0, cosine=0;
};
[[nodiscard]] Result<EnvironmentDescription> validate_cloud_reference(const EnvironmentDescription&, const CloudReferenceSettings&, std::size_t rays);
[[nodiscard]] Result<CloudPreparedRay> prepare_cloud_ray(const EnvironmentDescription&, const CloudRay&);
} // namespace dk::render::detail
