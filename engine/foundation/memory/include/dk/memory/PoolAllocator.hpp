#pragma once
#include <dk/memory/Pool.hpp>
#include <type_traits>
#include <utility>

namespace dk::memory {
// Explicit shared-pool ownership; default construction is empty, never TLS routed.
template<class T> class PoolAllocator {
public:
    using value_type = T;
    using propagate_on_container_copy_assignment = std::true_type;
    using propagate_on_container_move_assignment = std::true_type;
    using propagate_on_container_swap = std::true_type;
    using is_always_equal = std::false_type;
    PoolAllocator() noexcept = default;
    explicit PoolAllocator(SharedPoolResource resource) noexcept : resource_(std::move(resource)) {}
    PoolAllocator(const PoolAllocator&) noexcept = default;
    PoolAllocator& operator=(const PoolAllocator&) noexcept = default;
    PoolAllocator(PoolAllocator&& other) noexcept : resource_(other.resource_) {}
    PoolAllocator& operator=(PoolAllocator&& other) noexcept { resource_ = other.resource_; return *this; }
    template<class U> PoolAllocator(const PoolAllocator<U>& other) noexcept : resource_(other.resource()) {}
    [[nodiscard]] T* allocate(std::size_t count)
    {
        auto bytes = checked_byte_size(count, sizeof(T));
        if (!bytes) { throw std::bad_array_new_length{}; }
        auto result = resource_.try_allocate(*bytes, alignof(T));
        if (!result) { throw std::bad_alloc{}; }
        return reinterpret_cast<T*>(::new (*result) std::byte[*bytes]);
    }
    void deallocate(T* pointer, std::size_t count) noexcept
    {
        auto bytes = checked_byte_size(count, sizeof(T));
        if (!bytes) { std::terminate(); }
        resource_.deallocate(pointer, *bytes, alignof(T));
    }
    [[nodiscard]] const SharedPoolResource& resource() const noexcept { return resource_; }
    template<class U> [[nodiscard]] bool operator==(const PoolAllocator<U>& other) const noexcept
    { return resource_ == other.resource(); }
private:
    SharedPoolResource resource_;
};
} // namespace dk::memory
