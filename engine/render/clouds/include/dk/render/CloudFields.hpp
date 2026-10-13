#pragma once
#include <dk/scene/Environment.hpp>
#include <memory>
#include <span>
#include <vector>

namespace dk::render {
struct CloudFieldKey {
    std::uint32_t seed, generator_version, weather_resolution, base_resolution, detail_resolution;
    bool operator==(const CloudFieldKey&) const = default;
};
[[nodiscard]] CloudFieldKey cloud_field_key(const CloudParameters&) noexcept;
enum class CloudField { weather, base, detail };
class CloudFieldData {
public:
    CloudFieldData() = default;
    CloudFieldData(const CloudFieldData&) = delete;
    CloudFieldData& operator=(const CloudFieldData&) = delete;
    [[nodiscard]] CloudFieldKey key() const noexcept { return key_; }
    [[nodiscard]] std::uint64_t generation() const noexcept { return generation_; }
    [[nodiscard]] std::uint64_t hash() const noexcept { return hash_; }
    [[nodiscard]] std::size_t bytes() const noexcept;
    [[nodiscard]] std::span<const std::uint8_t> texels(CloudField) const noexcept;
    // Normalized repeat coordinates, texel-center bilinear/trilinear sampling.
    [[nodiscard]] Result<std::array<double, 2>> sample(CloudField, EnvironmentVector uvw) const;
private:
    friend class CloudFieldCache;
    CloudFieldKey key_{};
    std::uint64_t generation_ = 0, hash_ = 0;
    std::vector<std::uint8_t> weather_, base_, detail_;
};
struct CloudFieldLimits {
    std::size_t bytes = 8 * 1024 * 1024;
    std::uint32_t generations = 2;
};
// Single-threaded owner; held shared_ptrs count toward the next prepare's budget.
class CloudFieldCache {
public:
    explicit CloudFieldCache(CloudFieldLimits limits = {}) : limits_(limits) {}
    CloudFieldCache(const CloudFieldCache&) = delete;
    CloudFieldCache& operator=(const CloudFieldCache&) = delete;
    [[nodiscard]] Result<std::shared_ptr<const CloudFieldData>> prepare(const CloudParameters&);
    [[nodiscard]] std::shared_ptr<const CloudFieldData> current() const { return current_; }
private:
    CloudFieldLimits limits_;
    std::shared_ptr<const CloudFieldData> current_;
    std::vector<std::weak_ptr<const CloudFieldData>> live_;
};
// Returns dimensionless density [0, clouds.density], with explicit signed seconds.
[[nodiscard]] Result<double> sample_cloud_density(const EnvironmentDescription&, const CloudFieldData&,
    const EnvironmentVector& world_position_m, double time_seconds);
} // namespace dk::render
