#include <dk/assets/GltfImporter.hpp>
#include <dk/io/File.hpp>
#include <dk/memory/Arena.hpp>
#include <dk/profiling/Profiler.hpp>
#include <fastgltf/core.hpp>
#include <fastgltf/tools.hpp>
#include "ImportInternal.hpp"
#include "StbImageDecoder.hpp"
#include <algorithm>
#include <map>
#include <set>

namespace dk {
namespace {
using namespace import_detail;
using Json = nlohmann::json;
std::uint32_t u32(std::span<const std::byte> bytes, std::size_t offset)
{
    range(offset, 4, bytes.size(), "GLB word"); std::uint32_t value = 0;
    for (unsigned i = 0; i < 4; ++i) { value |= std::to_integer<std::uint32_t>(bytes[offset + i]) << (i * 8); } return value;
}
struct Container { std::string_view json; std::span<const std::byte> binary; };
Container container(std::span<const std::byte> bytes)
{
    if (bytes.size() < 4 || u32(bytes, 0) != 0x46546c67) { return {{reinterpret_cast<const char*>(bytes.data()), bytes.size()}, {}}; }
    require(bytes.size() >= 20 && u32(bytes, 4) == 2 && u32(bytes, 8) == bytes.size(), "Invalid GLB header");
    const auto json_size = u32(bytes, 12); require(json_size % 4 == 0 && u32(bytes, 16) == 0x4e4f534a, "Invalid GLB JSON chunk");
    range(20, json_size, bytes.size(), "GLB JSON"); Container result{{reinterpret_cast<const char*>(bytes.data() + 20), json_size}, {}};
    const auto next = std::size_t{20} + json_size;
    if (next != bytes.size()) {
        range(next, 8, bytes.size(), "GLB BIN header"); const auto size = u32(bytes, next);
        require(size % 4 == 0 && u32(bytes, next + 4) == 0x004e4942, "Invalid GLB BIN chunk");
        range(next + 8, size, bytes.size(), "GLB BIN"); require(next + 8 + size == bytes.size(), "Unexpected GLB chunks");
        result.binary = bytes.subspan(next + 8, size);
    }
    return result;
}
std::filesystem::path normalized(std::string_view name)
{
    require(!name.empty() && name.size() <= 4096 && name.find_first_of("\\:\0", 0, 3) == name.npos, "Invalid source path");
    auto path = take(path_from_utf8(name)); require(!path.has_root_path(), "Source must be project-relative");
    require(path.lexically_normal() == path && name.back() != '/', "Source path must be normalized");
    for (const auto& part : path) { require(part != "." && part != "..", "Source path escapes project"); } return path;
}
std::string resolve_uri(const ProjectPaths& paths, const std::filesystem::path& source, std::string_view uri)
{
    require(!uri.empty() && uri.size() <= 4096 && uri.find_first_of(":?#\\") == uri.npos, "Unsupported URI: " + std::string{uri}, ErrorCode::not_supported);
    std::string decoded;
    for (std::size_t i = 0; i < uri.size(); ++i) {
        if (uri[i] != '%') { decoded.push_back(uri[i]); continue; }
        require(i + 2 < uri.size(), "Truncated URI escape");
        const auto hex = [](char c) -> int { if (c >= '0' && c <= '9') return c - '0'; if (c >= 'a' && c <= 'f') return c - 'a' + 10; if (c >= 'A' && c <= 'F') return c - 'A' + 10; return -1; };
        const auto a = hex(uri[i + 1]), b = hex(uri[i + 2]); require(a >= 0 && b >= 0, "Invalid URI escape");
        decoded.push_back(static_cast<char>((a << 4) | b)); i += 2;
    }
    require(decoded.find_first_of(":?#\\\0", 0, 5) == decoded.npos, "Invalid decoded URI");
    const auto relative = take(path_from_utf8(decoded)); require(!relative.has_root_path(), "Absolute URI rejected");
    const auto combined = (source.parent_path() / relative).lexically_normal();
    (void)take(paths.resolve(combined)); const auto text = take(path_to_utf8(combined)); (void)normalized(text); return text;
}
Json parse(std::string_view text)
{
    std::vector<std::set<std::string>> keys;
    auto json = Json::parse(text, [&](int depth, Json::parse_event_t event, Json& value) {
        require(depth <= 64, "glTF JSON nesting exceeds limit");
        if (event == Json::parse_event_t::object_start) { keys.emplace_back(); }
        if (event == Json::parse_event_t::key) { require(keys.back().insert(value.get<std::string>()).second, "Duplicate glTF key"); }
        if (event == Json::parse_event_t::object_end) { keys.pop_back(); }
        return true;
    });
    require(json.is_object() && json.contains("asset") && json["asset"].is_object() && json["asset"].value("version", "") == "2.0", "Expected glTF 2.0");
    for (const auto key : {"extensionsUsed", "extensionsRequired", "skins", "animations"}) {
        if (json.contains(key)) { require(json[key].is_array(), "Expected glTF array"); require(json[key].empty(), std::string{key} + " is unsupported", ErrorCode::not_supported); }
    }
    // Reject extension objects even when the producer forgot extensionsUsed.
    const auto inspect = [&](auto&& self, const Json& value) -> void {
        if (value.is_object()) { for (auto it = value.begin(); it != value.end(); ++it) {
            if (it.key() == "extensions" || it.key() == "sparse" || it.key() == "targets" || it.key() == "weights" || it.key() == "skin") {
                require(false, it.key() + " is unsupported", ErrorCode::not_supported);
            }
            if (it.key() != "extras") { self(self, it.value()); }
        } } else if (value.is_array()) { for (const auto& item : value) { self(self, item); } }
    }; inspect(inspect, json);
    for (const auto key : {"meshes", "materials", "textures", "images", "samplers", "buffers", "bufferViews", "accessors", "nodes", "scenes"}) {
        if (json.contains(key)) { require(json[key].is_array() && json[key].size() <= 10000, "Invalid or excessive " + std::string{key}); }
    }
    return json;
}
void limits(const ImportLimits& value)
{
    const ImportLimits hard;
    const std::size_t ImportLimits::* fields[]{&ImportLimits::source_bytes, &ImportLimits::dependency_bytes, &ImportLimits::input_bytes,
        &ImportLimits::output_bytes, &ImportLimits::vertices, &ImportLimits::indices, &ImportLimits::primitives,
        &ImportLimits::image_dimension, &ImportLimits::image_bytes, &ImportLimits::texture_bytes};
    for (const auto field : fields) { require(value.*field > 0 && value.*field <= hard.*field, "Invalid import limit"); }
}
class Importer {
public:
    const ProjectPaths& paths;
    const GltfImportRequest& request;
    ImportResult result;
    std::size_t input_used = 0, output_used = 0, vertices_used = 0, indices_used = 0;
    std::size_t texture_used = 0;
    std::map<std::size_t, AssetId> textures;
    std::map<std::string, std::size_t, std::less<>> inputs;
    std::pmr::set<std::string, std::less<>> used_keys{memory::current_scratch_resource()};
    std::string location = "source";
    Importer(const ProjectPaths& p, const GltfImportRequest& r) : paths(p), request(r) {}
    std::span<const std::byte> read(std::string_view name, std::size_t limit)
    {
        if (const auto found = inputs.find(name); found != inputs.end()) { return result.inputs[found->second].bytes; }
        const auto path = take(paths.resolve(normalized(name)));
        std::error_code error; const auto status = std::filesystem::status(path, error);
        require(error != std::errc::no_such_file_or_directory && (error || std::filesystem::exists(status)), "Missing dependency: " + std::string{name}, ErrorCode::not_found);
        require(!error, "Cannot inspect dependency: " + std::string{name}, ErrorCode::io_error);
        require(std::filesystem::is_regular_file(status), "Expected regular dependency: " + std::string{name});
        const auto remaining = request.limits.input_bytes - input_used;
        auto bytes = take(read_file_bytes(path, std::min(limit, remaining)));
        consume(input_used, bytes.size(), request.limits.input_bytes, "input");
        const auto index = result.inputs.size(); result.inputs.push_back({owned(name), Vector<std::byte>{bytes.begin(), bytes.end()}});
        inputs.emplace(name, index); return result.inputs.back().bytes;
    }
    AssetId identity(std::string key, AssetKind kind)
    {
        require(result.outputs.size() < 10000, "Output count exceeds limit"); used_keys.insert(key);
        AssetId id;
        const auto found = std::find_if(request.identities.begin(), request.identities.end(), [&](const auto& value) { return std::string_view{value.key} == key; });
        if (found != request.identities.end()) { require(found->kind == kind, "Output kind changed: " + key, ErrorCode::conflict); id = found->id; }
        else { id = take(AssetId::generate()); }
        result.outputs.push_back({owned(key), id, kind}); return id;
    }
    const fastgltf::Accessor& accessor(const fastgltf::Asset& asset, std::size_t index, fastgltf::AccessorType type, bool indices = false)
    {
        location += "/accessor/" + std::to_string(index);
        require(index < asset.accessors.size(), "Accessor index out of range"); const auto& a = asset.accessors[index];
        require(a.type == type && !a.normalized, "Invalid accessor type or normalization");
        const auto component = a.componentType;
        require(indices ? (component == fastgltf::ComponentType::UnsignedByte || component == fastgltf::ComponentType::UnsignedShort || component == fastgltf::ComponentType::UnsignedInt)
            : component == fastgltf::ComponentType::Float, "Unsupported accessor component", ErrorCode::not_supported);
        require(!a.sparse && a.bufferViewIndex.has_value(), "Sparse or absent accessor bufferView", ErrorCode::not_supported);
        require(a.count > 0 && *a.bufferViewIndex < asset.bufferViews.size(), "Invalid accessor count or view");
        const auto& view = asset.bufferViews[*a.bufferViewIndex];
        const auto width = static_cast<std::size_t>(fastgltf::getComponentByteSize(component));
        const auto element = width * static_cast<std::size_t>(fastgltf::getNumComponents(type));
        const auto stride = view.byteStride.value_or(element);
        require(stride >= element && stride % width == 0 && (!view.byteStride || (stride >= 4 && stride <= 252 && stride % 4 == 0)), "Invalid accessor stride");
        require(!indices || !view.byteStride, "Index buffer must not be interleaved");
        require(a.byteOffset % width == 0 && view.byteOffset % width == 0, "Unaligned accessor");
        require(indices || (view.byteOffset + a.byteOffset) % 4 == 0, "Unaligned vertex accessor");
        range(a.byteOffset, element, view.byteLength, "accessor element");
        require(a.count - 1 <= (view.byteLength - a.byteOffset - element) / stride, "Accessor exceeds bufferView");
        return a;
    }
    AssetId texture(const Json& json, const fastgltf::Asset& asset, const std::filesystem::path& source, std::size_t index)
    {
        if (const auto found = textures.find(index); found != textures.end()) { return found->second; }
        location = "texture/" + std::to_string(index);
        const auto& definition = json.at("textures").at(index);
        const auto image_index = definition.at("source").get<std::size_t>();
        location += "/image/" + std::to_string(image_index);
        const auto& image = json.at("images").at(image_index); std::span<const std::byte> bytes; std::string origin;
        if (image.contains("uri")) {
            require(!image.contains("bufferView"), "Image has multiple sources");
            origin = resolve_uri(paths,source,image["uri"].get<std::string>()); bytes = read(origin,request.limits.dependency_bytes);
        } else {
            require(image.contains("mimeType"), "Embedded image MIME required");
            const auto view_index = image.at("bufferView").get<std::size_t>(); require(view_index < asset.bufferViews.size(), "Image bufferView out of range");
            const auto& view = asset.bufferViews[view_index]; require(!view.byteStride, "Image bufferView must not have stride");
            const auto& buffer = std::get<fastgltf::sources::ByteView>(asset.buffers[view.bufferIndex].data).bytes;
            bytes = std::span<const std::byte>{buffer.data(),buffer.size()}.subspan(view.byteOffset,view.byteLength);
            origin = std::string{request.source} + "#image/" + std::to_string(image_index);
        }
        auto decoded = decode_image(bytes,origin,image.value("mimeType",std::string{}),request.limits,texture_used,output_used);
        if (definition.contains("sampler")) {
            const auto& sampler = json.at("samplers").at(definition["sampler"].get<std::size_t>());
            const auto filter = [](unsigned value, bool min) { require(value == 9728 || value == 9729 || (min && value >= 9984 && value <= 9987), "Invalid sampler filter"); return static_cast<TextureFilter>(value); };
            const auto wrap = [](unsigned value) { require(value == 10497 || value == 33071 || value == 33648, "Invalid sampler wrap"); return static_cast<TextureWrap>(value); };
            if (sampler.contains("minFilter")) { decoded.sampler.min_filter = filter(sampler["minFilter"].get<unsigned>(),true); }
            if (sampler.contains("magFilter")) { decoded.sampler.mag_filter = filter(sampler["magFilter"].get<unsigned>(),false); }
            decoded.sampler.wrap_s = wrap(sampler.value("wrapS",10497U)); decoded.sampler.wrap_t = wrap(sampler.value("wrapT",10497U));
        }
        decoded.id = identity("texture/" + std::to_string(index),AssetKind::texture);
        const auto id = decoded.id; result.textures.push_back(std::move(decoded)); textures.emplace(index,id); return id;
    }
    ImportResult run()
    {
        limits(request.limits); require(std::isfinite(request.unit_scale) && request.unit_scale > 0, "Invalid unit_scale");
        require(request.identities.size() <= 10000, "Too many identities"); std::set<std::string> keys; std::set<AssetId> ids;
        for (const auto& item : request.identities) { require(!item.id.is_nil() && keys.insert(std::string{item.key}).second && ids.insert(item.id).second, "Invalid or duplicate output identity"); }
        const auto source = normalized(request.source); auto extension = take(path_to_utf8(source.extension()));
        std::transform(extension.begin(), extension.end(), extension.begin(), [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c; });
        require(extension == ".gltf" || extension == ".glb", "Expected glTF/GLB source", ErrorCode::not_supported);
        const auto source_bytes = read(request.source, request.limits.source_bytes); const auto decoded = container(source_bytes);
        const auto json = parse(decoded.json);
        require(json.contains("meshes") && json["meshes"].size() == 1, "Exactly one mesh is supported", ErrorCode::not_supported);
        require(json["meshes"][0].contains("primitives") && json["meshes"][0]["primitives"].is_array()
            && !json["meshes"][0]["primitives"].empty() && json["meshes"][0]["primitives"].size() <= request.limits.primitives, "Invalid primitive count");
        // Reject data/network/absolute URIs before the third-party parser can decode anything.
        for (const auto key : {"buffers", "images"}) { if (json.contains(key)) { for (const auto& item : json[key]) {
            if (item.contains("uri")) { (void)resolve_uri(paths, source, item["uri"].get<std::string>()); }
        } } }
        if (json.contains("buffers")) { for (const auto& buffer : json["buffers"]) {
            require(buffer.contains("byteLength") && buffer["byteLength"].is_number_unsigned()
                && buffer["byteLength"].get<std::uint64_t>() > 0
                && buffer["byteLength"].get<std::uint64_t>() <= request.limits.dependency_bytes, "Invalid declared buffer byteLength");
        } }
        auto data = fastgltf::GltfDataBuffer::FromBytes(source_bytes.data(), source_bytes.size());
        require(data.error() == fastgltf::Error::None, "Cannot prepare glTF bytes");
        fastgltf::Parser parser;
        auto parsed = parser.loadGltf(data.get(), {}, fastgltf::Options::None);
        require(parsed.error() == fastgltf::Error::None, "fastgltf: " + std::string{fastgltf::getErrorMessage(parsed.error())});
        auto& asset = parsed.get();
        for (std::size_t i = 0; i < asset.buffers.size(); ++i) {
            location = "buffer/" + std::to_string(i); std::span<const std::byte> bytes;
            const auto& raw = json.at("buffers").at(i);
            if (raw.contains("uri")) { bytes = read(resolve_uri(paths, source, raw["uri"].get<std::string>()), request.limits.dependency_bytes); }
            else { require(i == 0 && !decoded.binary.empty(), "Missing embedded buffer"); bytes = decoded.binary; }
            require(asset.buffers[i].byteLength > 0 && asset.buffers[i].byteLength <= bytes.size(), "Truncated buffer");
            asset.buffers[i].data = fastgltf::sources::ByteView{fastgltf::span<const std::byte>{bytes.data(), asset.buffers[i].byteLength}};
        }
        for (std::size_t i = 0; i < asset.bufferViews.size(); ++i) {
            location = "bufferView/" + std::to_string(i); const auto& view = asset.bufferViews[i];
            require(view.bufferIndex < asset.buffers.size() && view.byteLength > 0, "Invalid bufferView");
            range(view.byteOffset, view.byteLength, asset.buffers[view.bufferIndex].byteLength, location);
        }
        result.unit_scale = request.unit_scale; result.mesh.id = identity("mesh/0", AssetKind::mesh);
        std::map<std::size_t, AssetId> materials;
        for (std::size_t p = 0; p < asset.meshes[0].primitives.size(); ++p) {
            DK_PROFILE_ZONE("Assets.DecodePrimitive"); location = "mesh/0/primitive/" + std::to_string(p);
            const auto& primitive = asset.meshes[0].primitives[p]; require(primitive.type == fastgltf::PrimitiveType::Triangles, "Only TRIANGLES supported", ErrorCode::not_supported);
            for (const auto& attr : primitive.attributes) { require(attr.name == "POSITION" || attr.name == "NORMAL" || attr.name == "TEXCOORD_0", "Unsupported attribute: " + std::string{attr.name}, ErrorCode::not_supported); }
            const auto position = primitive.findAttribute("POSITION"); require(position != primitive.attributes.end(), "POSITION required");
            const auto& a = accessor(asset, position->accessorIndex, fastgltf::AccessorType::Vec3);
            consume(vertices_used, a.count, request.limits.vertices, "vertices");
            consume(output_used, product(a.count, sizeof(Vec3f), request.limits.output_bytes, "positions"), request.limits.output_bytes, "output");
            MeshPrimitive out; out.positions.resize(a.count);
            fastgltf::iterateAccessorWithIndex<fastgltf::math::fvec3>(asset, a, [&](auto value, std::size_t i) {
                for (std::size_t c = 0; c < 3; ++c) {
                    const double scaled = static_cast<double>(value[c]) * request.unit_scale;
                    require(std::isfinite(scaled) && std::abs(scaled) <= std::numeric_limits<float>::max(), "Nonfinite or overflowing position");
                    out.positions[i][static_cast<Eigen::Index>(c)] = static_cast<float>(scaled);
                }
                if (i == 0) { out.bounds_min = out.bounds_max = out.positions[i]; }
                else { out.bounds_min = out.bounds_min.cwiseMin(out.positions[i]); out.bounds_max = out.bounds_max.cwiseMax(out.positions[i]); }
            });
            const auto normal = primitive.findAttribute("NORMAL");
            if (normal != primitive.attributes.end()) {
                const auto& n = accessor(asset, normal->accessorIndex, fastgltf::AccessorType::Vec3); require(n.count == a.count, "NORMAL count differs");
                consume(output_used, product(n.count, sizeof(Vec3f), request.limits.output_bytes, "normals"), request.limits.output_bytes, "output"); out.normals.resize(n.count);
                fastgltf::iterateAccessorWithIndex<fastgltf::math::fvec3>(asset, n, [&](auto v, std::size_t i) { out.normals[i] = Vec3f{v[0], v[1], v[2]}; require(out.normals[i].allFinite(), "Nonfinite normal"); });
            }
            const auto uv = primitive.findAttribute("TEXCOORD_0");
            if (uv != primitive.attributes.end()) {
                const auto& t = accessor(asset, uv->accessorIndex, fastgltf::AccessorType::Vec2); require(t.count == a.count, "TEXCOORD_0 count differs");
                consume(output_used, product(t.count, sizeof(Vec2f), request.limits.output_bytes, "UVs"), request.limits.output_bytes, "output"); out.texcoords.resize(t.count);
                fastgltf::iterateAccessorWithIndex<fastgltf::math::fvec2>(asset, t, [&](auto v, std::size_t i) { out.texcoords[i] = Vec2f{v[0], v[1]}; require(out.texcoords[i].allFinite(), "Nonfinite UV"); });
            }
            if (primitive.indicesAccessor) {
                const auto& index = accessor(asset, *primitive.indicesAccessor, fastgltf::AccessorType::Scalar, true);
                consume(indices_used, index.count, request.limits.indices, "indices"); require(index.count % 3 == 0, "Incomplete triangle");
                consume(output_used, product(index.count, 4, request.limits.output_bytes, "indices"), request.limits.output_bytes, "output"); out.indices.resize(index.count);
                fastgltf::iterateAccessorWithIndex<std::uint32_t>(asset, index, [&](auto value, std::size_t i) { require(value < a.count, "Index exceeds vertex count"); out.indices[i] = value; });
            } else {
                consume(indices_used, a.count, request.limits.indices, "indices"); require(a.count % 3 == 0, "Incomplete unindexed triangle");
                consume(output_used, product(a.count, 4, request.limits.output_bytes, "indices"), request.limits.output_bytes, "output"); out.indices.resize(a.count);
                for (std::size_t i = 0; i < a.count; ++i) { out.indices[i] = static_cast<std::uint32_t>(i); }
            }
            if (primitive.materialIndex) {
                const auto index = *primitive.materialIndex; require(index < asset.materials.size(), "Material index out of range");
                if (!materials.contains(index)) {
                    location = "material/" + std::to_string(index); const auto& m = asset.materials[index];
                    require(!m.normalTexture && !m.occlusionTexture && !m.emissiveTexture && !m.pbrData.metallicRoughnessTexture, "Unsupported material texture channel", ErrorCode::not_supported);
                    MaterialData material; material.id = identity(location, AssetKind::material);
                    if (m.pbrData.baseColorTexture) {
                        require(m.pbrData.baseColorTexture->texCoordIndex == 0, "Only UV0 supported", ErrorCode::not_supported);
                        material.base_color_texture = texture(json,asset,source,m.pbrData.baseColorTexture->textureIndex);
                    }
                    for (std::size_t c = 0; c < 4; ++c) { material.base_color[static_cast<Eigen::Index>(c)] = m.pbrData.baseColorFactor[c]; }
                    for (std::size_t c = 0; c < 3; ++c) { material.emissive[static_cast<Eigen::Index>(c)] = m.emissiveFactor[c]; }
                    material.metallic = m.pbrData.metallicFactor; material.roughness = m.pbrData.roughnessFactor; material.alpha_cutoff = m.alphaCutoff;
                    require(material.base_color.allFinite() && (material.base_color.array() >= 0).all() && (material.base_color.array() <= 1).all()
                        && material.emissive.allFinite() && (material.emissive.array() >= 0).all() && (material.emissive.array() <= 1).all()
                        && std::isfinite(material.metallic) && material.metallic >= 0 && material.metallic <= 1
                        && std::isfinite(material.roughness) && material.roughness >= 0 && material.roughness <= 1
                        && std::isfinite(material.alpha_cutoff) && material.alpha_cutoff >= 0, "Invalid material factors");
                    material.double_sided = m.doubleSided;
                    material.alpha_mode = m.alphaMode == fastgltf::AlphaMode::Opaque ? AlphaMode::opaque : m.alphaMode == fastgltf::AlphaMode::Mask ? AlphaMode::mask : AlphaMode::blend;
                    materials.emplace(index, material.id); result.materials.push_back(std::move(material));
                }
                out.material = materials.at(index);
                require(!asset.materials[index].pbrData.baseColorTexture || !out.texcoords.empty(), "baseColorTexture requires TEXCOORD_0");
            }
            result.mesh.primitives.push_back(std::move(out));
        }
        for (const auto& mapping : request.identities) { require(used_keys.contains(std::string{mapping.key}), "Previously mapped output is missing: " + std::string{mapping.key}, ErrorCode::not_found); }
        if (!asset.nodes.empty()) { result.diagnostics.push_back(owned("Only mesh-local data imported; node placements are not applied")); }
        std::sort(result.outputs.begin(), result.outputs.end(), [](const auto& a, const auto& b) { return a.key < b.key; });
        return std::move(result);
    }
};
}
Result<ImportResult> import_gltf(const ProjectPaths& paths, const GltfImportRequest& request)
{
    DK_PROFILE_ZONE("Assets.ImportGltf");
    memory::ScratchScope scratch;
    Importer importer{paths, request};
    try { return importer.run(); }
    catch (const Failure& error) { return std::unexpected(error.error.with_context(std::string{request.source} + ": " + importer.location)); }
    catch (const Json::exception& error) { return std::unexpected(Error{ErrorCode::invalid_argument, error.what(), {std::string{request.source}, importer.location}}); }
}
} // namespace dk
