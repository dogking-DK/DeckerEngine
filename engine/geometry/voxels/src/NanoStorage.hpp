#pragma once

#include <dk/geometry/SparseVoxels.hpp>
#ifdef _MSC_VER
// NanoVDB 32.7.0 toGridType<T> has a fallback return after an if-constexpr return.
// MSVC reports C4702 even in /external:W0 headers; keep the suppression scoped to the SDK.
#pragma warning(push)
#pragma warning(disable: 4702)
#endif
#include <nanovdb/GridHandle.h>
#ifdef _MSC_VER
#pragma warning(pop)
#endif
#include <stdexcept>

namespace dk::geometry::voxels::detail {
// NanoVDB calls create() with its computed exact output size before allocating/writing.
// Limit state is per conversion; no process-global allocator or mutable budget.
class LimitedNanoBuffer {
public:
    explicit LimitedNanoBuffer(std::uint64_t maximum = 0) : maximum_(maximum) {}
    LimitedNanoBuffer(LimitedNanoBuffer&&) = default;
    LimitedNanoBuffer& operator=(LimitedNanoBuffer&&) = default;
    LimitedNanoBuffer(const LimitedNanoBuffer&) = delete;
    LimitedNanoBuffer& operator=(const LimitedNanoBuffer&) = delete;
    static LimitedNanoBuffer create(std::uint64_t bytes, const LimitedNanoBuffer* context = nullptr) {
        if (!context || bytes > context->maximum_) throw std::length_error("NanoVDB output exceeds byte budget");
        LimitedNanoBuffer result(context->maximum_);
        result.buffer_ = nanovdb::HostBuffer::create(bytes);
        return result;
    }
    void* data() { return buffer_.data(); }
    const void* data() const { return buffer_.data(); }
    std::uint64_t size() const { return buffer_.size(); }
    void clear() { buffer_.clear(); }
private:
    std::uint64_t maximum_;
    nanovdb::HostBuffer buffer_;
};
struct SparseStorage {
    nanovdb::GridHandle<LimitedNanoBuffer> handle;
    Domain domain;
    Transformd corner_to_local;
    SparseStatistics statistics;
};
struct SparseFactory {
    static SparseGrid publish(std::shared_ptr<const SparseStorage> storage) { return SparseGrid(std::move(storage)); }
};
} // namespace dk::geometry::voxels::detail
