#pragma once
#include <dk/render/CloudFields.hpp>
namespace dk::render::detail {
// Internal loop: environment, key, coordinates and time have already been checked.
[[nodiscard]] double cloud_density_at(const EnvironmentDescription&,const CloudFieldData&,const EnvironmentVector&,double time,double height);
}
