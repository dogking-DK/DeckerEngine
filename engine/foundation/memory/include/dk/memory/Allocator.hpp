#pragma once
#include <dk/memory/Context.hpp>
#include <new>
#include <type_traits>
#include <utility>

namespace dk::memory {
template<class T> class Allocator {
public:
    using value_type = T;
    using propagate_on_container_copy_assignment = std::true_type;
    using propagate_on_container_move_assignment = std::true_type;
    using propagate_on_container_swap = std::true_type;
    using is_always_equal = std::false_type;

    Allocator() : resource_(current_resource()) {}
    explicit Allocator(ResourceHandle resource) noexcept : resource_(std::move(resource)) {}
    Allocator(const Allocator&) noexcept = default;
    Allocator& operator=(const Allocator&) noexcept = default;
    // Allocator moves preserve source equality and allow moved-from containers to be reused.
    Allocator(Allocator&& other) noexcept : resource_(other.resource_) {}
    Allocator& operator=(Allocator&& other) noexcept { resource_ = other.resource_; return *this; }
    template<class U> Allocator(const Allocator<U>& other) noexcept : resource_(other.resource()) {}

    [[nodiscard]] T* allocate(std::size_t count)
    {
        auto bytes = checked_byte_size(count, sizeof(T));
        if (!bytes) { throw std::bad_array_new_length{}; }
        auto result = resource_.try_allocate(*bytes, alignof(T));
        if (!result) { throw std::bad_alloc{}; }
        // A placement byte array implicitly creates the T array without constructing elements.
        return reinterpret_cast<T*>(::new (*result) std::byte[*bytes]);
    }
    void deallocate(T* pointer, std::size_t count) noexcept
    {
        auto bytes = checked_byte_size(count, sizeof(T));
        if (!bytes) { std::terminate(); } // Count must match a successful allocate.
        resource_.deallocate(pointer, *bytes, alignof(T));
    }
    [[nodiscard]] const ResourceHandle& resource() const noexcept { return resource_; }
    template<class U> [[nodiscard]] bool operator==(const Allocator<U>& other) const noexcept
    { return resource_ == other.resource(); }
private:
    ResourceHandle resource_;
};
} // namespace dk::memory
