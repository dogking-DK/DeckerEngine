#pragma once

#include <dk/geometry/Voxels.hpp>
#include <memory>

namespace dk::geometry::voxels {
namespace detail { struct SparseStorage; struct SparseFactory; }
struct SparseStatistics {
    std::uint64_t source_tree_bytes = 0, normalized_tree_bytes = 0, nano_bytes = 0;
    std::uint64_t normalization_visits = 0, active_voxels = 0;
    std::array<std::uint32_t, 3> node_counts{}; // leaf, lower, upper
};

// Copies share an immutable, aligned NanoVDB allocation. Each query has private traversal state.
// No public arbitrary-byte decoder: construction is through the validated CPU converter.
class SparseGrid final {
public:
    [[nodiscard]] std::uint32_t value(Coord) const noexcept;
    [[nodiscard]] Result<QueryResult> nearest(const Ray&, const Transformd& local_to_world = {}, QueryLimits = {}) const;
    [[nodiscard]] const Domain& domain() const noexcept;
    [[nodiscard]] const Transformd& corner_to_local() const noexcept;
    [[nodiscard]] const SparseStatistics& statistics() const noexcept;
    // Borrowed immutable bytes, valid while this grid or a copy is alive.
    [[nodiscard]] std::span<const std::byte> bytes() const noexcept;
private:
    friend struct detail::SparseFactory;
    explicit SparseGrid(std::shared_ptr<const detail::SparseStorage> storage) : storage_(std::move(storage)) {}
    std::shared_ptr<const detail::SparseStorage> storage_;
};
} // namespace dk::geometry::voxels
