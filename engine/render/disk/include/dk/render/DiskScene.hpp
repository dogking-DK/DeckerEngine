#pragma once
#include <dk/render/RenderScene.hpp>
#include <dk/render/GpuAssets.hpp>
#include <dk/assets/GltfImporter.hpp>
#include <dk/scene/Project.hpp>

namespace dk::render {
namespace detail { struct DiskSceneState; }
struct DiskSceneOptions {
    GltfImportProfile profile = GltfImportProfile::strict;
    std::size_t max_assets = 64;
    std::size_t cpu_bytes = 512U * 1024U * 1024U;
};
// Read-only CPU candidate. Requires a bound ThreadContext with scratch.
// No meta/cache/project writes; loading again never mutates an existing candidate.
class DiskScene final {
public:
    DiskScene() = default;
    [[nodiscard]] explicit operator bool() const noexcept { return bool(state_); }
    [[nodiscard]] static Result<DiskScene> load(memory::ResourceHandle, const Project&, const DiskSceneOptions& = {});
    [[nodiscard]] RenderScene scene() const noexcept;
    [[nodiscard]] std::span<const std::shared_ptr<const CpuAsset>> assets() const noexcept;
    // Fresh cache; uploads and waits each package. Earlier GPU submissions cannot be undone
    // on later failure, but no partial cache is returned and all candidates are released.
    [[nodiscard]] Result<GpuAssets> upload(graphics::SubmissionQueue&) const;
private:
    explicit DiskScene(std::shared_ptr<const detail::DiskSceneState> state) : state_(std::move(state)) {}
    std::shared_ptr<const detail::DiskSceneState> state_;
};
} // namespace dk::render
