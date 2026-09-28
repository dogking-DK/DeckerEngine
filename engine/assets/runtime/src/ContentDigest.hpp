#pragma once
#include <dk/core/Result.hpp>
#include <array>
#include <filesystem>
#include <memory>
#include <span>

namespace dk::asset_detail {
struct ContentDigest {
    std::array<std::byte, 16> bytes{};
    bool operator==(const ContentDigest&) const = default;
    [[nodiscard]] std::string hex() const;
};
[[nodiscard]] ContentDigest content_digest(std::span<const std::byte> bytes);
class ContentHasher {
public:
    ContentHasher();
    ~ContentHasher();
    ContentHasher(const ContentHasher&) = delete;
    ContentHasher& operator=(const ContentHasher&) = delete;
    void update(std::span<const std::byte> bytes);
    [[nodiscard]] ContentDigest digest() const;
private:
    struct State;
    std::unique_ptr<State> state_;
};
[[nodiscard]] Result<ContentDigest> file_digest(const std::filesystem::path& path);
} // namespace dk::asset_detail
