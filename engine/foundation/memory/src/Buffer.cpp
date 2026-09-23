#include <dk/memory/Buffer.hpp>
#include <algorithm>
#include <cstring>
#include <utility>

namespace dk::memory {
Buffer::Buffer(ResourceHandle resource, void* pointer, std::size_t size, std::size_t alignment) noexcept
    : resource_(std::move(resource)), data_(static_cast<std::byte*>(pointer)), size_(size), alignment_(alignment) {}
Buffer::~Buffer() { reset(); }
Buffer::Buffer(Buffer&& other) noexcept { swap(other); }
Buffer& Buffer::operator=(Buffer&& other) noexcept
{
    if (this != &other) { Buffer old{std::move(other)}; swap(old); }
    return *this;
}
void Buffer::reset() noexcept
{
    resource_.deallocate(data_, size_, alignment_);
    data_ = nullptr; size_ = 0; alignment_ = alignof(std::max_align_t); resource_ = {};
}
void Buffer::swap(Buffer& other) noexcept
{
    using std::swap;
    swap(resource_, other.resource_); swap(data_, other.data_);
    swap(size_, other.size_); swap(alignment_, other.alignment_);
}
std::expected<Buffer, AllocationError> try_allocate(ResourceHandle resource, std::size_t bytes, std::size_t alignment) noexcept
{
    auto result = resource.try_allocate(bytes, alignment);
    if (!result) { return std::unexpected(result.error()); }
    return Buffer{std::move(resource), *result, bytes, alignment};
}
std::expected<void, AllocationError> Buffer::try_resize(std::size_t bytes) noexcept
{
    if (!resource_) { return std::unexpected(AllocationError::invalid_handle); }
    if (bytes == size_) { return {}; }
    auto candidate = try_allocate(resource_, bytes, alignment_);
    if (!candidate) { return std::unexpected(candidate.error()); }
    if (const auto length = (std::min)(size_, bytes); length != 0) {
        std::memcpy(candidate->data_, data_, length);
    }
    swap(*candidate);
    return {};
}
} // namespace dk::memory
