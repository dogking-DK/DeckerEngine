#pragma once
#include <dk/graphics/Pipeline.hpp>
#include <variant>

namespace dk::graphics {
namespace detail { struct BindingState; }
struct BufferBinding { const Buffer* buffer = nullptr; vk::DeviceSize offset = 0, size = VK_WHOLE_SIZE; };
struct ImageBinding { const ImageView* view = nullptr; vk::ImageLayout layout = vk::ImageLayout::eShaderReadOnlyOptimal; };
struct SamplerBinding { const Sampler* sampler = nullptr; };
// One array element per write. A set must be completely initialized.
struct BindingWrite {
    std::uint32_t binding = 0, array_element = 0;
    std::variant<BufferBinding, ImageBinding, SamplerBinding> resource;
};
class BindingSet final {
public:
    BindingSet() = default;
    BindingSet(BindingSet&&) noexcept = default;
    BindingSet& operator=(BindingSet&&) noexcept = default;
    BindingSet(const BindingSet&) = delete;
    BindingSet& operator=(const BindingSet&) = delete;
    [[nodiscard]] explicit operator bool() const noexcept { return bool(state_); }
    [[nodiscard]] vk::DescriptorSet handle() const noexcept;
    [[nodiscard]] std::uint32_t set_index() const noexcept;
private:
    friend class ResourceFactory;
    friend struct detail::ObjectAccess;
    explicit BindingSet(std::shared_ptr<detail::BindingState> state) : state_(std::move(state)) {}
    std::shared_ptr<detail::BindingState> state_;
};
} // namespace dk::graphics
