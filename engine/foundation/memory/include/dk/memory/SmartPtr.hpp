#pragma once
#include <dk/memory/Allocator.hpp>
#include <tuple>
#include <utility>

namespace dk::memory {
template<class T> struct ResourceDeleter {
    ResourceHandle resource;
    void operator()(T* pointer) const noexcept
    {
        if (!pointer) { return; }
        std::destroy_at(pointer);
        resource.deallocate(const_cast<std::remove_cv_t<T>*>(pointer), sizeof(T), alignof(T));
    }
};
template<class T> using UniquePtr = std::unique_ptr<T, ResourceDeleter<T>>;

template<class T, class... Args> requires (!std::is_array_v<T>)
[[nodiscard]] UniquePtr<T> make_unique_in(ResourceHandle resource, Args&&... args)
{
    Allocator<std::remove_cv_t<T>> allocator{resource};
    auto* storage = allocator.allocate(1);
    T* pointer = nullptr;
    try {
        // Placement new also supports const T; construct_at requires cv-unqualified T.
        pointer = std::apply([&](auto&&... constructor_args) {
            return ::new (static_cast<void*>(storage)) T(std::forward<decltype(constructor_args)>(constructor_args)...);
        }, std::uses_allocator_construction_args<T>(allocator, std::forward<Args>(args)...));
    }
    catch (...) { allocator.deallocate(storage, 1); throw; }
    return UniquePtr<T>{pointer, ResourceDeleter<T>{std::move(resource)}};
}
template<class T, class... Args> requires (!std::is_array_v<T>)
[[nodiscard]] UniquePtr<T> make_unique(Args&&... args)
{ return make_unique_in<T>(current_resource(), std::forward<Args>(args)...); }

template<class T, class... Args> requires (!std::is_array_v<T>)
[[nodiscard]] std::shared_ptr<T> make_shared_in(ResourceHandle resource, Args&&... args)
{
    const Allocator<std::remove_cv_t<T>> allocator{std::move(resource)};
    return std::apply([&](auto&&... constructor_args) {
        return std::allocate_shared<T>(allocator, std::forward<decltype(constructor_args)>(constructor_args)...);
    }, std::uses_allocator_construction_args<T>(allocator, std::forward<Args>(args)...));
}
template<class T, class... Args> requires (!std::is_array_v<T>)
[[nodiscard]] std::shared_ptr<T> make_shared(Args&&... args)
{ return make_shared_in<T>(current_resource(), std::forward<Args>(args)...); }
} // namespace dk::memory
