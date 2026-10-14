#pragma once
#include <dk/render/PathTraceReference.hpp>

namespace dk::render::detail {
struct PathBsdf {
    Vec3d base_color;
    double metallic = 0, roughness = 1;
    PathTraceMaterialModel model = PathTraceMaterialModel::lambert;
};
struct PathBsdfEval { Vec3d value = Vec3d::Zero(); double pdf = 0; };
struct PathBsdfSample { Vec3d direction = Vec3d::Zero(); PathBsdfEval evaluated; };
// Unit directions point away from the surface. Invalid hemispheres have zero mass.
[[nodiscard]] PathBsdfEval eval_path_bsdf(const PathBsdf&, const Vec3d& normal, const Vec3d& wo, const Vec3d& wi);
[[nodiscard]] PathBsdfSample sample_path_bsdf(const PathBsdf&, const Vec3d& normal, const Vec3d& wo, double u, double v);
}
