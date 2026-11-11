#pragma once

#include <dk/geometry/Voxels.hpp>
#include <algorithm>
#include <cmath>
#include <limits>

namespace dk::geometry::voxels::detail {
using WideCoord = std::array<std::int64_t, 3>;
struct Region { std::uint32_t value; std::uint32_t dimension; };

inline bool contains(const Domain& domain, const WideCoord& p) {
    for (int i = 0; i < 3; ++i)
        if (p[i] < domain.minimum[i] || p[i] > domain.maximum[i]) return false;
    return true;
}
inline Coord narrow(const WideCoord& p) {
    return {static_cast<std::int32_t>(p[0]), static_cast<std::int32_t>(p[1]), static_cast<std::int32_t>(p[2])};
}
// Floor division, including negative indices; no implementation-defined signed shifts.
inline std::int64_t block_min(std::int64_t value, std::uint32_t size) {
    const auto d = static_cast<std::int64_t>(size);
    return (value / d - (value % d < 0 ? 1 : 0)) * d;
}

template<class Sample>
Result<QueryResult> traverse(const Ray& world_ray, const Domain& domain,
    const Transformd& corner_to_local, const Transformd& local_to_world, QueryLimits limits, Sample&& sample) {
    if (auto valid = geometry::validate(world_ray); !valid) return std::unexpected(valid.error());
    if (!limits.maximum_regions) return std::unexpected(Error{ErrorCode::invalid_argument, "Voxel query work limit must be positive"});
    auto world = local_to_world.compose(corner_to_local);
    if (!world) return std::unexpected(world.error());
    auto inverse = world->inverse();
    if (!inverse) return std::unexpected(inverse.error());
    auto origin = inverse->transform_point(world_ray.origin);
    auto direction = inverse->transform_direction(world_ray.direction);
    if (!origin) return std::unexpected(origin.error());
    if (!direction) return std::unexpected(direction.error());
    const Ray ray{*origin, *direction, world_ray.minimum, world_ray.maximum};
    if (auto valid = geometry::validate(ray); !valid) return std::unexpected(valid.error());
    if (ray.origin.cwiseAbs().maxCoeff() > 0x1p40)
        return std::unexpected(Error{ErrorCode::invalid_argument, "Voxel ray origin exceeds the supported index-space precision range"});

    double enter = -std::numeric_limits<double>::infinity(), leave = std::numeric_limits<double>::infinity();
    std::array<double, 3> entry{}, exit{};
    QueryResult result;
    for (int i = 0; i < 3; ++i) {
        const double lo = domain.minimum[i], hi = static_cast<double>(domain.maximum[i]) + 1;
        if (ray.direction[i] == 0) {
            // Half-open ownership: rays in an outer face plane only graze the volume.
            if (ray.origin[i] <= lo || ray.origin[i] >= hi) return result;
            entry[i] = -std::numeric_limits<double>::infinity();
            exit[i] = std::numeric_limits<double>::infinity();
        } else {
            entry[i] = (lo - ray.origin[i]) / ray.direction[i];
            exit[i] = (hi - ray.origin[i]) / ray.direction[i];
            if (!std::isfinite(entry[i]) || !std::isfinite(exit[i]))
                return std::unexpected(Error{ErrorCode::invalid_argument, "Voxel ray exceeds numeric range"});
            if (entry[i] > exit[i]) std::swap(entry[i], exit[i]);
        }
        enter = std::max(enter, entry[i]);
        leave = std::min(leave, exit[i]);
    }
    if (enter >= leave || leave < ray.minimum || enter > ray.maximum) return result;
    double time = std::max(enter, ray.minimum);
    const double end = std::min(leave, ray.maximum);

    const auto point_at = [&](double t) {
        Vec3d p;
        for (int i = 0; i < 3; ++i) p[i] = std::fma(t, ray.direction[i], ray.origin[i]);
        return p;
    };
    const auto cells_at = [&](const Vec3d& p, bool after) {
        WideCoord cell{};
        for (int i = 0; i < 3; ++i) {
            const double f = std::floor(p[i]);
            cell[i] = static_cast<std::int64_t>(f);
            if (p[i] == f && ((after && ray.direction[i] < 0) || (!after && ray.direction[i] > 0))) --cell[i];
        }
        return cell;
    };
    const auto read = [&](const WideCoord& p) -> std::uint32_t {
        return contains(domain, p) ? sample(narrow(p)).value : 0;
    };
    const auto transition = [&](const Vec3d& point, double t) -> std::optional<Hit> {
        const auto before = cells_at(point, false), after = cells_at(point, true);
        const auto a = read(before), b = read(after);
        if ((a == 0) == (b == 0)) return {};
        int axis = 0;
        while (axis < 3 && before[axis] == after[axis]) ++axis;
        if (axis == 3) return {};
        const bool leaving = a != 0;
        // A simultaneous internal cell-plane crossing does not define a surface normal.
        for (int candidate = axis; candidate < 3; ++candidate) {
            if (before[candidate] == after[candidate]) continue;
            auto neighbor = leaving ? before : after;
            const bool outward = leaving ? ray.direction[candidate] > 0 : ray.direction[candidate] < 0;
            neighbor[candidate] += outward ? 1 : -1;
            if (read(neighbor) == 0) { axis = candidate; break; }
        }
        const bool positive = leaving ? ray.direction[axis] > 0 : ray.direction[axis] < 0;
        Vec3d normal = Vec3d::Zero();
        normal[axis] = positive ? 1 : -1;
        normal = (inverse->matrix().template block<3, 3>(0, 0).transpose() * normal).eval();
        normal /= normal.cwiseAbs().maxCoeff(); // Avoid norm overflow/underflow at extreme affine scales.
        normal.normalize();
        return Hit{t, narrow(leaving ? before : after), static_cast<Face>(axis * 2 + (positive ? 1 : 0)), leaving ? a : b, normal};
    };
    auto point = point_at(time);
    // Pin slab planes exactly, instead of moving the ray by a world-space epsilon.
    for (int i = 0; i < 3; ++i) {
        if (time == entry[i]) point[i] = ray.direction[i] > 0 ? domain.minimum[i] : static_cast<double>(domain.maximum[i]) + 1;
        if (time == exit[i]) point[i] = ray.direction[i] > 0 ? static_cast<double>(domain.maximum[i]) + 1 : domain.minimum[i];
    }
    if (auto hit = transition(point, time)) { result.hit = *hit; return result; }
    auto cell = cells_at(point, true);
    while (contains(domain, cell) && time < end) {
        if (result.visited_regions == limits.maximum_regions)
            return std::unexpected(Error{ErrorCode::invalid_argument, "Voxel traversal work limit exceeded"});
        ++result.visited_regions;
        const auto region = sample(narrow(cell));
        std::array<double, 3> next{}, plane{};
        for (int i = 0; i < 3; ++i) {
            const auto low = std::max<std::int64_t>(block_min(cell[i], region.dimension), domain.minimum[i]);
            const auto high = std::min<std::int64_t>(block_min(cell[i], region.dimension) + region.dimension,
                static_cast<std::int64_t>(domain.maximum[i]) + 1);
            plane[i] = static_cast<double>(ray.direction[i] > 0 ? high : low);
            next[i] = ray.direction[i] == 0 ? std::numeric_limits<double>::infinity() : (plane[i] - ray.origin[i]) / ray.direction[i];
        }
        const double next_time = std::min({next[0], next[1], next[2]});
        if (!(next_time > time)) return std::unexpected(Error{ErrorCode::invalid_argument, "Voxel traversal cannot advance at this numeric scale"});
        if (next_time > end) return result;
        point = point_at(next_time);
        for (int i = 0; i < 3; ++i) if (next[i] == next_time) point[i] = plane[i];
        if (auto hit = transition(point, next_time)) { result.hit = *hit; return result; }
        cell = cells_at(point, true);
        time = next_time;
    }
    return result;
}
} // namespace dk::geometry::voxels::detail
