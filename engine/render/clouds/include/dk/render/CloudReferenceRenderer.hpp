#pragma once
#include <dk/render/CloudReference.hpp>
#include <dk/render/CloudTextures.hpp>
#include <filesystem>

namespace dk::render {
namespace detail { struct CloudReferenceState; }
// One queue, externally serialized. Synchronous full-sample ray batches; no history.
// Textures are borrowed until return. Every successful call owns its CPU results.
class CloudReferenceRenderer final {
public:
    CloudReferenceRenderer() = default;
    CloudReferenceRenderer(CloudReferenceRenderer&&) noexcept = default;
    CloudReferenceRenderer& operator=(CloudReferenceRenderer&&) noexcept = default;
    CloudReferenceRenderer(const CloudReferenceRenderer&) = delete;
    CloudReferenceRenderer& operator=(const CloudReferenceRenderer&) = delete;
    [[nodiscard]] static Result<CloudReferenceRenderer> create(memory::ResourceHandle, graphics::SubmissionQueue&, const std::filesystem::path& shader_directory);
    [[nodiscard]] Result<std::vector<CloudIntegral>> integrate(graphics::SubmissionQueue&, const EnvironmentDescription&,
        const CloudTextures&, std::span<const CloudRay>, const CloudReferenceSettings& = {});
private:
    explicit CloudReferenceRenderer(std::shared_ptr<detail::CloudReferenceState> state) : state_(std::move(state)) {}
    std::shared_ptr<detail::CloudReferenceState> state_;
};
} // namespace dk::render
