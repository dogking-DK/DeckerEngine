#include <dk/render/GpuAssets.hpp>
#include <algorithm>
#include <cmath>
#include <limits>

namespace dk::render {
namespace {
Result<void> invalid(const char* text) { return std::unexpected(Error{ErrorCode::invalid_argument, text}); }
bool min_filter(TextureFilter f) {
    switch (f) {
    case TextureFilter::nearest: case TextureFilter::linear:
    case TextureFilter::nearest_mipmap_nearest: case TextureFilter::linear_mipmap_nearest:
    case TextureFilter::nearest_mipmap_linear: case TextureFilter::linear_mipmap_linear: return true;
    }
    return false;
}
bool wrap(TextureWrap value) {
    return value == TextureWrap::clamp || value == TextureWrap::mirrored_repeat || value == TextureWrap::repeat;
}
bool unit(float value) { return std::isfinite(value) && value >= 0 && value <= 1; }
}
Result<void> validate_gpu_asset(const CpuAsset& asset) {
    if (asset.mesh.id.is_nil() || asset.mesh.primitives.empty()) return invalid("GPU mesh requires an ID and primitives");
    for (const auto& primitive : asset.mesh.primitives) {
        const auto count = primitive.positions.size();
        if (!count || count > std::numeric_limits<std::uint32_t>::max() || primitive.indices.empty() ||
            primitive.indices.size() > std::numeric_limits<std::uint32_t>::max() || primitive.indices.size() % 3 != 0)
            return invalid("GPU primitive requires nonempty uint32-addressable triangle data");
        if ((!primitive.normals.empty() && primitive.normals.size() != count) ||
            (!primitive.texcoords.empty() && primitive.texcoords.size() != count))
            return invalid("GPU primitive attribute counts differ");
        for (const auto& position : primitive.positions) if (!position.allFinite()) return invalid("nonfinite mesh position");
        for (const auto& normal : primitive.normals) if (!normal.allFinite()) return invalid("nonfinite mesh normal");
        for (const auto& uv : primitive.texcoords) if (!uv.allFinite()) return invalid("nonfinite mesh UV");
        for (auto index : primitive.indices) if (index >= count) return invalid("mesh index out of bounds");
        if (primitive.material && !std::ranges::any_of(asset.materials, [&](const auto& m) { return m.id == *primitive.material; }))
            return invalid("mesh material is not in this CPU asset");
    }
    for (std::size_t i = 0; i < asset.materials.size(); ++i) {
        const auto& m = asset.materials[i];
        if (m.id.is_nil() || m.id == asset.mesh.id) return invalid("invalid or duplicate material ID");
        for (std::size_t j = 0; j < i; ++j) if (asset.materials[j].id == m.id) return invalid("duplicate material ID");
        if (!m.base_color.allFinite() || (m.base_color.array() < 0).any() || (m.base_color.array() > 1).any() ||
            !m.emissive.allFinite() || (m.emissive.array() < 0).any() || !unit(m.metallic) || !unit(m.roughness) || !unit(m.alpha_cutoff))
            return invalid("invalid material numeric value");
        if (m.alpha_mode != AlphaMode::opaque && m.alpha_mode != AlphaMode::mask && m.alpha_mode != AlphaMode::blend)
            return invalid("invalid material alpha mode");
        if (m.base_color_texture && !std::ranges::any_of(asset.textures, [&](const auto& t) { return t.id == *m.base_color_texture; }))
            return invalid("material texture is not in this CPU asset");
    }
    for (std::size_t i = 0; i < asset.textures.size(); ++i) {
        const auto& t = asset.textures[i];
        if (t.id.is_nil() || t.id == asset.mesh.id || std::ranges::any_of(asset.materials, [&](const auto& m) { return m.id == t.id; }))
            return invalid("invalid or duplicate texture ID");
        for (std::size_t j = 0; j < i; ++j) if (asset.textures[j].id == t.id) return invalid("duplicate texture ID");
        if (!t.width || !t.height || static_cast<std::uint64_t>(t.width) * t.height > std::numeric_limits<std::size_t>::max() / 4 ||
            static_cast<std::uint64_t>(t.width) * t.height * 4 != t.rgba8.size())
            return invalid("texture requires tightly packed RGBA8 pixels and nonzero extent");
        const auto& s = t.sampler;
        if ((s.min_filter && !min_filter(*s.min_filter)) ||
            (s.mag_filter && *s.mag_filter != TextureFilter::nearest && *s.mag_filter != TextureFilter::linear) ||
            !wrap(s.wrap_s) || !wrap(s.wrap_t)) return invalid("invalid texture sampler");
    }
    return {};
}
} // namespace dk::render
