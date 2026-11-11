#pragma once
#include <dk/geometry/Voxels.hpp>
#include <cmath>

// Independent small-grid oracle: enumerate exposed faces and intersect planes.
// Intentionally excludes edges/corners; those have separate analytic expectations.
inline std::optional<dk::geometry::voxels::Hit> voxel_face_oracle(
    const dk::geometry::voxels::DenseGrid& grid, const dk::geometry::Ray& ray) {
    using namespace dk;
    using namespace dk::geometry::voxels;
    std::optional<Hit> best;
    const auto& domain = grid.domain();
    for (int z = domain.minimum[2]; z <= domain.maximum[2]; ++z)
    for (int y = domain.minimum[1]; y <= domain.maximum[1]; ++y)
    for (int x = domain.minimum[0]; x <= domain.maximum[0]; ++x) {
        const Coord cell{x, y, z};
        const auto value = grid.value(cell);
        if (!value) continue;
        for (int axis = 0; axis < 3; ++axis) for (int positive = 0; positive < 2; ++positive) {
            auto neighbor = cell; neighbor[axis] += positive ? 1 : -1;
            if (grid.value(neighbor) || ray.direction[axis] == 0) continue;
            const double t = (cell[axis] + positive - ray.origin[axis]) / ray.direction[axis];
            if (t < ray.minimum || t > ray.maximum || (best && t >= best->distance)) continue;
            const Vec3d p = ray.origin + t * ray.direction;
            bool interior = true;
            for (int other = 0; other < 3; ++other)
                if (other != axis && !(p[other] > cell[other] && p[other] < cell[other] + 1)) interior = false;
            if (!interior) continue;
            Vec3d normal = Vec3d::Zero(); normal[axis] = positive ? 1 : -1;
            best = Hit{t, cell, static_cast<Face>(axis * 2 + positive), value, normal};
        }
    }
    return best;
}
