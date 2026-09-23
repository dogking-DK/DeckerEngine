#pragma once
#include <dk/memory/PoolAllocator.hpp>
#include <tuple>

namespace dk::memory {
template<class T, class Pool> struct PoolDeleter {
    static_assert(std::is_same_v<Pool, LocalPoolResource> || std::is_same_v<Pool, SharedPoolResource>);
    using Owner = std::conditional_t<std::is_same_v<Pool, LocalPoolResource>, LocalPoolResource*, SharedPoolResource>;
    Owner owner{};
    void operator()(T* pointer) const noexcept
    {
        if (!pointer) { return; }
        if constexpr (std::is_same_v<Pool, LocalPoolResource>) {
            if (!owner || !owner->on_owner_thread()) { std::terminate(); }
            std::destroy_at(pointer);
            owner->deallocate(const_cast<std::remove_cv_t<T>*>(pointer), sizeof(T), alignof(T));
        } else {
            std::destroy_at(pointer);
            owner.deallocate(const_cast<std::remove_cv_t<T>*>(pointer), sizeof(T), alignof(T));
        }
    }
};

template<class T, class Pool = LocalPoolResource> class ObjectPool {
    static_assert(!std::is_array_v<T> && std::is_nothrow_destructible_v<T>);
    static constexpr bool local = std::is_same_v<Pool, LocalPoolResource>;
public:
    using Deleter = PoolDeleter<T, Pool>;
    using Pointer = std::unique_ptr<T, Deleter>;
    explicit ObjectPool(LocalPoolResource& pool) noexcept requires(local) : owner_(&pool) {}
    explicit ObjectPool(SharedPoolResource pool) noexcept requires(!local) : owner_(std::move(pool)) {}
    template<class... Args> [[nodiscard]] Pointer make(Args&&... args) const
    {
        const auto allocate = [&] {
            if constexpr (local) { return owner_->try_allocate(sizeof(T), alignof(T)); }
            else { return owner_.try_allocate(sizeof(T), alignof(T)); }
        };
        auto storage = allocate();
        if (!storage) { throw PoolError{storage.error()}; }
        try {
            auto allocator = object_allocator();
            auto* pointer = std::apply([&](auto&&... constructor_args) {
                return ::new (*storage) T(std::forward<decltype(constructor_args)>(constructor_args)...);
            }, std::uses_allocator_construction_args<T>(allocator, std::forward<Args>(args)...));
            return Pointer{pointer, Deleter{owner_}};
        } catch (...) {
            if constexpr (local) { owner_->deallocate(*storage, sizeof(T), alignof(T)); }
            else { owner_.deallocate(*storage, sizeof(T), alignof(T)); }
            throw;
        }
    }
    template<class... Args> [[nodiscard]] std::shared_ptr<T> make_shared(Args&&... args) const requires(!local)
    {
        const auto allocator = object_allocator();
        return std::apply([&](auto&&... constructor_args) {
            return std::allocate_shared<T>(allocator, std::forward<decltype(constructor_args)>(constructor_args)...);
        }, std::uses_allocator_construction_args<T>(allocator, std::forward<Args>(args)...));
    }
private:
    auto object_allocator() const
    {
        if constexpr (local) { return std::pmr::polymorphic_allocator<std::remove_cv_t<T>>{owner_->pmr_resource()}; }
        else { return PoolAllocator<std::remove_cv_t<T>>{owner_}; }
    }
    typename Deleter::Owner owner_;
};
template<class T> using SharedObjectPool = ObjectPool<T, SharedPoolResource>;
} // namespace dk::memory
