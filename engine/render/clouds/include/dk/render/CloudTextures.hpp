#pragma once
#include <dk/render/CloudFields.hpp>
#include <dk/graphics/Transfer.hpp>

namespace dk::render {
class CloudTextures {
public:
    [[nodiscard]] CloudFieldKey key() const noexcept { return key_; }
    [[nodiscard]] std::uint64_t generation() const noexcept { return generation_; }
    [[nodiscard]] std::uint64_t hash() const noexcept { return hash_; }
    [[nodiscard]] std::size_t bytes() const noexcept { return bytes_; }
    [[nodiscard]] const graphics::Image& image(CloudField field) const { return images_.at(static_cast<std::size_t>(field)); }
    [[nodiscard]] const graphics::ImageView& view(CloudField field) const { return views_.at(static_cast<std::size_t>(field)); }
    [[nodiscard]] const graphics::Sampler& sampler() const noexcept { return sampler_; }
private:
    friend class CloudTextureCache;
    CloudFieldKey key_{};
    std::uint64_t generation_ = 0, hash_ = 0;
    std::size_t bytes_ = 0;
    std::array<graphics::Image,3> images_;
    std::array<graphics::ImageView,3> views_;
    graphics::Sampler sampler_;
};
struct CloudTextureLimits {
    std::size_t bytes = 16 * 1024 * 1024; // Live images + candidate images + candidate staging payload.
    std::uint32_t generations = 2;
};
class CloudTextureCache {
public:
    [[nodiscard]] static Result<std::unique_ptr<CloudTextureCache>> create(graphics::SubmissionQueue&, CloudTextureLimits = {});
    CloudTextureCache(const CloudTextureCache&) = delete;
    CloudTextureCache& operator=(const CloudTextureCache&) = delete;
    [[nodiscard]] Result<std::shared_ptr<const CloudTextures>> prepare(graphics::SubmissionQueue&, const CloudFieldData&);
    [[nodiscard]] std::shared_ptr<const CloudTextures> current() const { return current_; }
private:
    CloudTextureCache(CloudTextureLimits limits, graphics::Buffer identity) : limits_(limits), identity_(std::move(identity)) {}
    CloudTextureLimits limits_;
    graphics::Buffer identity_;
    std::shared_ptr<const CloudTextures> current_;
    // Retired generations remain charged until the queue drains, even if the
    // caller releases its snapshot after recording/submitting borrowed images.
    std::vector<std::shared_ptr<const CloudTextures>> live_;
};
} // namespace dk::render
