#pragma once
#include <dk/assets/AssetReference.hpp>
#include <dk/math/Types.hpp>
#include <dk/memory/Containers.hpp>
#include <optional>

namespace dk {
struct OutputIdentity { String key; AssetId id; AssetKind kind = AssetKind::mesh; };
struct InputSnapshot { String path; Vector<std::byte> bytes; };
struct MeshPrimitive {
    Vector<Vec3f> positions, normals; // Empty normals/UVs explicitly mean absent.
    Vector<Vec2f> texcoords;
    Vector<std::uint32_t> indices;
    Vec3f bounds_min = Vec3f::Zero(), bounds_max = Vec3f::Zero();
    std::optional<AssetId> material; // Absent means the built-in MaterialData defaults.
};
struct MeshData { AssetId id; Vector<MeshPrimitive> primitives; };
enum class AlphaMode { opaque, mask, blend };
struct MaterialData {
    AssetId id;
    Vec4f base_color = Vec4f::Ones();
    Vec3f emissive = Vec3f::Zero();
    float metallic = 1, roughness = 1, alpha_cutoff = 0.5f;
    AlphaMode alpha_mode = AlphaMode::opaque;
    bool double_sided = false;
    std::optional<AssetId> base_color_texture;
};
enum class TextureFilter : std::uint32_t { nearest = 9728, linear = 9729, nearest_mipmap_nearest = 9984,
    linear_mipmap_nearest = 9985, nearest_mipmap_linear = 9986, linear_mipmap_linear = 9987 };
enum class TextureWrap : std::uint32_t { clamp = 33071, mirrored_repeat = 33648, repeat = 10497 };
struct TextureSampler {
    std::optional<TextureFilter> min_filter, mag_filter;
    TextureWrap wrap_s = TextureWrap::repeat, wrap_t = TextureWrap::repeat;
};
struct TextureData {
    AssetId id;
    std::uint32_t width = 0, height = 0;
    Vector<std::byte> rgba8; // sRGB, top row first, unchanged UV convention.
    TextureSampler sampler;
    String origin;
};
struct CpuAsset {
    MeshData mesh;
    Vector<MaterialData> materials;
    Vector<TextureData> textures;
    Vector<OutputIdentity> outputs;
    Vector<String> diagnostics;
    double unit_scale = 1;
};
struct ImportResult : CpuAsset {
    Vector<InputSnapshot> inputs; // Source first; exact owned bytes, no mutable parser views.
};
} // namespace dk
