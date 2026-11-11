#pragma once

#include <dk/geometry/Query.hpp>
#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace dk::geometry::voxels {

using Coord = std::array<std::int32_t, 3>;
// Inclusive integer cell bounds. Cell q occupies [q,q+1] in corner coordinates.
struct Domain { Coord minimum{}, maximum{}; };
inline constexpr std::int32_t coordinate_limit = 1 << 30;

struct DenseInput {
    Coord minimum{};
    std::array<std::uint32_t, 3> dimensions{};
    std::span<const std::uint32_t> values;
    std::uint32_t palette_size = 1;
    Transformd corner_to_local;
};
struct DenseLimits { std::uint64_t maximum_cells = 16 * 1024 * 1024; };
struct QueryLimits { std::uint64_t maximum_regions = 1024 * 1024; };
enum class Face : std::uint8_t { negative_x, positive_x, negative_y, positive_y, negative_z, positive_z };
struct Hit {
    double distance = 0;
    Coord cell{};
    Face face{};
    std::uint32_t value = 0;
    Vec3d normal = Vec3d::Zero(); // World normal; the cell and face remain in index space.
};
struct QueryResult {
    std::optional<Hit> hit;
    std::uint64_t visited_regions = 0;
};

[[nodiscard]] Result<void> validate(const Domain& domain);

// Immutable owning reference. Construction copies only after validation succeeds.
class DenseGrid final {
public:
    [[nodiscard]] static Result<DenseGrid> build(const DenseInput&, DenseLimits = {});
    [[nodiscard]] std::uint32_t value(Coord) const noexcept;
    [[nodiscard]] Result<QueryResult> nearest(const Ray&, const Transformd& local_to_world = {}, QueryLimits = {}) const;
    [[nodiscard]] const Domain& domain() const noexcept { return domain_; }
    [[nodiscard]] const Transformd& corner_to_local() const noexcept { return corner_to_local_; }
    [[nodiscard]] std::span<const std::uint32_t> values() const noexcept { return values_; }
    [[nodiscard]] std::uint32_t palette_size() const noexcept { return palette_size_; }
private:
    DenseGrid() = default;
    Domain domain_;
    std::array<std::uint32_t, 3> dimensions_{};
    std::vector<std::uint32_t> values_;
    std::uint32_t palette_size_ = 0;
    Transformd corner_to_local_;
};
} // namespace dk::geometry::voxels
