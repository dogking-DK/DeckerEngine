#pragma once
#include <dk/render/PathTraceReference.hpp>

namespace dk::render {
// Double-precision oracle, not an RTXDI packed-buffer ABI. Zero-weight draws count toward M.
class RisReservoir final {
public:
    static constexpr std::uint32_t invalid_sample = 0xffffffffu;
    // Returns whether this draw replaced the selection. Rejection leaves all state unchanged.
    [[nodiscard]] Result<bool> stream(double target, double proposal_pdf, double uniform);
    [[nodiscard]] std::uint32_t selected() const noexcept { return selected_; }
    [[nodiscard]] std::uint32_t candidates() const noexcept { return candidates_; }
    [[nodiscard]] double weight_sum() const noexcept { return sum_; }
    [[nodiscard]] double target() const noexcept { return target_; }
    // Final multiplier for the contribution density, not the marginal sampling PDF.
    [[nodiscard]] double normalization() const noexcept { return normalization_; }
private:
    std::uint32_t selected_ = invalid_sample, candidates_ = 0;
    double sum_ = 0, target_ = 0, normalization_ = 0;
};

// Each source is a properly weighted sample over its known support. A zero reservoir
// retains its effective M and support row even though its weight and target are zero.
struct RisReuseSource {
    double target = 0; // Target at the destination receiver.
    double weight = 0; // Already normalized source W.
    std::uint32_t candidates = 0; // Effective M, never image spp.
};
struct RisReuseSelection {
    std::uint32_t selected = RisReservoir::invalid_sample;
    std::uint64_t candidates = 0;
    double weight_sum = 0, normalization = 0;
};
// support[row * sources.size() + column]: source row supports the sample of column.
// Source membership/support must be fixed independently of the selected light samples;
// callers must cover the destination support, normally with fresh full-support candidates.
// This is merge math only, not a temporal/spatial renderer or a visibility cache.
[[nodiscard]] Result<RisReuseSelection> resample_ris_sources(std::span<const RisReuseSource>,
    std::span<const std::uint8_t> support, std::span<const double> uniforms);

struct RestirTemporalSettings {
    std::uint32_t version = 1, max_history_candidates = 64, max_history_age = 8;
    double position_tolerance = .1, normal_cosine = .95;
    bool operator==(const RestirTemporalSettings&) const = default;
};
struct RestirDiSettings {
    std::uint32_t version = 1, initial_candidates = 8; // 1..64 fresh candidates per epoch.
    std::uint64_t max_candidate_evaluations = std::uint64_t{1} << 24;
    std::optional<RestirTemporalSettings> temporal; // Full progressive GPU view only.
};
struct RestirReferenceEstimate : PathTraceEstimate {
    RestirDiSettings restir;
    PathTraceSamples samples;
    std::uint64_t candidate_evaluations = 0, selected_shadow_rays = 0;
};
enum class RestirRandomPurpose : std::uint32_t { light_class, light_index, light_u, light_v, replacement, temporal_replacement };
// Version 1 independent counter domain; BSDF/RR/camera retain path sampler v1.
[[nodiscard]] double restir_reference_sample(std::uint64_t seed, std::uint32_t pixel, std::uint32_t epoch,
    std::uint32_t candidate, RestirRandomPurpose) noexcept;
// First surface: RIS continuous lights + separate delta sun. Subsequent vertices: classic MIS.
// No GPU, history, denoising or SDK dependency. Requires path.estimator == mis.
[[nodiscard]] Result<RestirReferenceEstimate> estimate_restir_reference(const PathTraceScene&, const PathTraceRay&,
    const PathTraceSettings& = {}, const RestirDiSettings& = {}, PathTraceSamples = {},
    std::uint32_t pixel = 0, std::stop_token = {});
} // namespace dk::render
