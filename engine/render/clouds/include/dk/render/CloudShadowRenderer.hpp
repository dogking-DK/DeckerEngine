#pragma once
#include <dk/render/CloudShadow.hpp>
#include <dk/render/CloudTextures.hpp>
#include <filesystem>

namespace dk::render {
namespace detail { struct CloudShadowState; struct CloudShadowOwner; }
class ScenePipeline;
class CloudShadowMap final {
public:
    CloudShadowMap()=default;
    [[nodiscard]] explicit operator bool() const noexcept { return bool(state_); }
    [[nodiscard]] std::uint64_t generation() const noexcept;
    [[nodiscard]] graphics::Submission submission() const noexcept;
    [[nodiscard]] Result<EnvironmentDescription> environment() const;
    [[nodiscard]] Result<CloudShadowSettings> settings() const;
    [[nodiscard]] Result<CloudShadowProjection> projection() const;
    [[nodiscard]] Result<const graphics::Image*> image() const;
    [[nodiscard]] Result<void> read_transmittance(std::span<float>) const;
private:
    friend class CloudShadowRenderer;
    friend class ScenePipeline;
    void used_by(graphics::Submission) const noexcept;
    explicit CloudShadowMap(std::shared_ptr<detail::CloudShadowState> s):state_(std::move(s)) {}
    std::shared_ptr<detail::CloudShadowState> state_;
};
struct CloudShadowLimits { std::size_t bytes=64ULL*1024*1024; std::uint32_t generations=3; };
// Single queue, externally serialized. prepare records/submits without waiting;
// maps can immediately be consumed on that queue, readback requires completion.
class CloudShadowRenderer final {
public:
    CloudShadowRenderer()=default;
    CloudShadowRenderer(CloudShadowRenderer&&) noexcept=default;
    CloudShadowRenderer& operator=(CloudShadowRenderer&&) noexcept=default;
    CloudShadowRenderer(const CloudShadowRenderer&)=delete;
    CloudShadowRenderer& operator=(const CloudShadowRenderer&)=delete;
    [[nodiscard]] static Result<CloudShadowRenderer> create(memory::ResourceHandle,graphics::SubmissionQueue&,const std::filesystem::path&,CloudShadowLimits={});
    [[nodiscard]] Result<CloudShadowMap> prepare(graphics::SubmissionQueue&,const EnvironmentDescription&,const CloudTextures&,const CloudShadowSettings& = {});
    [[nodiscard]] CloudShadowMap current() const;
private:
    explicit CloudShadowRenderer(std::shared_ptr<detail::CloudShadowOwner> s):state_(std::move(s)) {}
    std::shared_ptr<detail::CloudShadowOwner> state_;
};
} // namespace dk::render
