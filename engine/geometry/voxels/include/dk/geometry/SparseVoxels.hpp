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
// Construction is through the CPU converter or the bounded, fully validated loader below.
class SparseGrid final {
public:
    [[nodiscard]] std::uint32_t value(Coord) const noexcept;
    [[nodiscard]] std::uint32_t maximum_value() const noexcept;
    [[nodiscard]] Result<QueryResult> nearest(const Ray&, const Transformd& local_to_world = {}, QueryLimits = {}) const;
    [[nodiscard]] const Domain& domain() const noexcept;
    [[nodiscard]] const Transformd& corner_to_local() const noexcept;
    [[nodiscard]] const SparseStatistics& statistics() const noexcept;
    // Borrowed immutable bytes, valid while this grid or a copy is alive.
    [[nodiscard]] std::span<const std::byte> bytes() const noexcept;
    // Layout v1: one conservative block per nonempty leaf or nonzero constant tile.
    // Inclusive cells, clipped to domain; does not expand constant tiles to voxels.
    [[nodiscard]] Result<std::vector<Domain>> surface_blocks(std::size_t maximum_blocks = 1024*1024) const;
private:
    friend struct detail::SparseFactory;
    explicit SparseGrid(std::shared_ptr<const detail::SparseStorage> storage) : storage_(std::move(storage)) {}
    std::shared_ptr<const detail::SparseStorage> storage_;
};
struct SparseLoadLimits { std::uint64_t bytes=256*1024*1024, values=64*1024*1024; };
// Accepts the fixed single-grid Int32 NanoVDB layout only. Copies into aligned storage;
// validates relative addresses and values before constructing any query accessor.
[[nodiscard]] Result<SparseGrid> load_sparse_grid(std::span<const std::byte>,const Domain&,
    const Transformd& corner_to_local,std::uint32_t palette_size,SparseLoadLimits = {});
[[nodiscard]] std::uint32_t sparse_layout_version() noexcept;
} // namespace dk::geometry::voxels
