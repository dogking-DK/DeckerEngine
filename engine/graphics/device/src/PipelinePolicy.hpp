#pragma once
#include "ObjectInternal.hpp"

namespace dk::graphics::detail {
struct ShaderLayoutView {
    ShaderStage stage;
    std::span<const ReflectedBinding> bindings;
    std::span<const ShaderPushConstant> push;
};
struct LayoutDescription {
    explicit LayoutDescription(memory::ResourceHandle resource)
        : bindings(memory::Allocator<LayoutBinding>{resource}), push(memory::Allocator<vk::PushConstantRange>{resource}) {}
    Vector<LayoutBinding> bindings;
    Vector<vk::PushConstantRange> push;
};
Result<LayoutDescription> merge_interfaces(memory::ResourceHandle resource, std::span<const ShaderLayoutView> shaders,
                                          const vk::PhysicalDeviceLimits& limits);
std::uint32_t vertex_format_size(vk::Format format) noexcept;
} // namespace dk::graphics::detail
