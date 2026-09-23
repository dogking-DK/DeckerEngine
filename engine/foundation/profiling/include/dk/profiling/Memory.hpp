#pragma once
#include <dk/profiling/Profiler.hpp>
#include <cstddef>

#ifndef DK_PROFILE_MEMORY
#define DK_PROFILE_MEMORY 0
#endif

namespace dk::profiling {
enum class HeapCategory { general, assets, scene, render, jobs, other };
[[nodiscard]] inline constexpr bool memory_enabled() noexcept
{
    return DK_ENABLE_PROFILING && DK_PROFILE_MEMORY;
}
#if DK_ENABLE_PROFILING && DK_PROFILE_MEMORY
// Pair using the original category. Call alloc before publication, free before
// returning storage. No logging, dk::memory allocations or user callbacks here.
void record_allocation(const void* pointer, std::size_t size, HeapCategory category) noexcept;
void record_free(const void* pointer, HeapCategory category) noexcept;
#else
inline void record_allocation(const void*, std::size_t, HeapCategory) noexcept {}
inline void record_free(const void*, HeapCategory) noexcept {}
#endif
} // namespace dk::profiling
