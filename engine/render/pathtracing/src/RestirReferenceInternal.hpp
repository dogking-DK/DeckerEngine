#pragma once
#include "PathReferenceInternal.hpp"
#include "PathBsdf.hpp"
#include <dk/render/RestirReference.hpp>

namespace dk::render::detail {
enum class RestirLightKind { none, triangle, environment };
struct RestirLightSample {
    RestirLightKind kind = RestirLightKind::none;
    std::uint64_t scene_fingerprint = 0;
    Vec3d environment_rgb = Vec3d::Zero(); // Together with scene identity freezes the light domain.
    std::size_t triangle = 0;
    Vec3d coordinates = Vec3d::Zero(); // Barycentrics for area measure, unit direction for solid angle.
    double proposal_pdf = 0; // Includes class selection; never includes a delta sun.
};
struct RestirLightEvaluation {
    Vec3d direction = Vec3d::Zero(), point = Vec3d::Zero(), contribution = Vec3d::Zero();
    double target = 0;
};
[[nodiscard]] RestirLightSample sample_restir_light(const PathSceneState&, const PathTraceSettings&, const Vec4d&);
[[nodiscard]] Result<RestirLightEvaluation> evaluate_restir_light(const PathSceneState&, const PathTraceSettings&,
    const PathTraceHit&, const PathBsdf&, const Vec3d& outgoing, const RestirLightSample&);
[[nodiscard]] bool path_visible(const PathSceneState&, const PathTraceHit&, const Vec3d&, PathTraceStatistics&,
    const PathTriangle* light = nullptr, const Vec3d& point = Vec3d::Zero());
[[nodiscard]] Result<Vec3d> restir_direct(const PathSceneState&, const PathTraceHit&, const PathBsdf&, const Vec3d&,
    const PathTraceSettings&, const RestirDiSettings&, std::uint32_t pixel, std::uint32_t sample,
    RestirReferenceEstimate&, std::stop_token);
} // namespace dk::render::detail
