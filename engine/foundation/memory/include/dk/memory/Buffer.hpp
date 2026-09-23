#pragma once
#include <dk/memory/Resource.hpp>
#include <span>

namespace dk::memory {
class Buffer;
[[nodiscard]] std::expected<Buffer, AllocationError> try_allocate(
    ResourceHandle resource, std::size_t bytes, std::size_t alignment = alignof(std::max_align_t)) noexcept;

// Owns untyped bytes. New bytes are uninitialized; resizing does not relocate C++ objects.
class Buffer {
public:
    Buffer() noexcept = default;
    ~Buffer();
    Buffer(Buffer&& other) noexcept;
    Buffer& operator=(Buffer&& other) noexcept;
    Buffer(const Buffer&) = delete;
    Buffer& operator=(const Buffer&) = delete;
    [[nodiscard]] std::byte* data() noexcept { return data_; }
    [[nodiscard]] const std::byte* data() const noexcept { return data_; }
    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    [[nodiscard]] std::size_t alignment() const noexcept { return alignment_; }
    [[nodiscard]] explicit operator bool() const noexcept { return data_ != nullptr; }
    [[nodiscard]] std::span<std::byte> bytes() noexcept { return {data_, size_}; }
    [[nodiscard]] std::span<const std::byte> bytes() const noexcept { return {data_, size_}; }
    [[nodiscard]] const ResourceHandle& resource() const noexcept { return resource_; }
    [[nodiscard]] std::expected<void, AllocationError> try_resize(std::size_t bytes) noexcept;
    void reset() noexcept;
    void swap(Buffer& other) noexcept;
private:
    friend std::expected<Buffer, AllocationError> try_allocate(ResourceHandle, std::size_t, std::size_t) noexcept;
    Buffer(ResourceHandle resource, void* pointer, std::size_t size, std::size_t alignment) noexcept;
    ResourceHandle resource_;
    std::byte* data_ = nullptr;
    std::size_t size_ = 0;
    std::size_t alignment_ = alignof(std::max_align_t);
};
inline void swap(Buffer& a, Buffer& b) noexcept { a.swap(b); }
} // namespace dk::memory
