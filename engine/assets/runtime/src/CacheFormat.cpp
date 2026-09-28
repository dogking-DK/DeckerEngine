#include "AssetCacheInternal.hpp"
#include <algorithm>
#include <bit>
#include <set>

namespace dk::asset_detail {
namespace {
using Json = nlohmann::json;
void fields(const Json& j, std::initializer_list<std::string_view> names)
{
    require(j.is_object() && j.size() == names.size(), "Invalid cache fields");
    for (const auto name : names) { require(j.contains(name), "Missing cache field: " + std::string{name}); }
}
std::string string(const Json& j) { require(j.is_string(), "Expected cache string"); return j.get<std::string>(); }
std::uint64_t integer(const Json& j)
{ require(j.is_number_unsigned(), "Expected unsigned cache integer"); return j.get<std::uint64_t>(); }
std::string digest(std::string_view s)
{
    require(s.size() == 32 && std::all_of(s.begin(), s.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }), "Invalid cache digest");
    return std::string{s};
}
std::string uuid(std::string_view s)
{
    auto value = take(AssetId::parse(s));
    require(!value.is_nil() && value.to_string() == s, "Noncanonical cache UUID"); return std::string{s};
}
Json parse(std::string_view text)
{
    require(text.size() <= cache_entry_limit, "Cache JSON byte limit");
    std::vector<std::set<std::string>> keys;
    return Json::parse(text, [&](int depth, Json::parse_event_t event, Json& value) {
        require(depth <= 64, "Cache JSON nesting limit");
        if (event == Json::parse_event_t::object_start) { keys.emplace_back(); }
        if (event == Json::parse_event_t::key) { require(keys.back().insert(string(value)).second, "Duplicate cache key"); }
        if (event == Json::parse_event_t::object_end) { keys.pop_back(); }
        return true;
    });
}
CacheDescriptor descriptor(const Json& j)
{
    fields(j, {"algorithm", "importer", "cache_version", "artifact_version", "implementation_version", "contract_version", "source", "meta", "inputs"});
    CacheDescriptor d;
    d.versions = {string(j["algorithm"]), string(j["importer"]), integer(j["cache_version"]),
        integer(j["artifact_version"]), integer(j["implementation_version"]), integer(j["contract_version"])};
    d.source = string(j["source"]); d.metadata = take(parse_asset_meta(j["meta"].dump()));
    require(j["inputs"].is_array() && j["inputs"].size() <= 20001, "Cache input count limit");
    for (const auto& input : j["inputs"]) {
        fields(input, {"path", "bytes", "digest"});
        d.inputs.push_back({owned(string(input["path"])), owned(digest(string(input["digest"]))), integer(input["bytes"])});
    }
    return d;
}
class KeyWriter {
    ContentHasher hasher_;
    void word(std::uint64_t n) {
        std::array<std::byte, 8> bytes{};
        for (unsigned i = 0; i < 8; ++i) { bytes[i] = static_cast<std::byte>((n >> (i * 8)) & 255); }
        hasher_.update(bytes);
    }
public:
    void field(unsigned char tag, std::string_view s) {
        const std::byte byte{tag}; hasher_.update({&byte, 1}); word(s.size());
        hasher_.update(std::as_bytes(std::span{s.data(), s.size()}));
    }
    void field(unsigned char tag, std::uint64_t n) {
        const std::byte byte{tag}; hasher_.update({&byte, 1}); word(8); word(n);
    }
    ContentDigest finish() const { return hasher_.digest(); }
};
}
Result<ContentDigest> cache_key(const CacheDescriptor& d)
{
    return attempt<ContentDigest>("cache_key", [&] {
        validate_meta(d.metadata); (void)relative_path(d.source);
        require(!d.versions.algorithm.empty() && d.versions.algorithm.size() <= 128
            && !d.versions.importer.empty() && d.versions.importer.size() <= 128, "Invalid cache version labels");
        (void)take(path_from_utf8(d.versions.algorithm)); (void)take(path_from_utf8(d.versions.importer));
        require(!d.inputs.empty() && d.inputs.size() <= 20001, "Invalid cache inputs");
        std::vector<const InputFingerprint*> inputs;
        for (const auto& input : d.inputs) { inputs.push_back(&input); }
        std::sort(inputs.begin(), inputs.end(), [](auto a, auto b) { return a->path < b->path; });
        std::uint64_t total = 0; bool source = false;
        for (std::size_t i = 0; i < inputs.size(); ++i) {
            const auto& input = *inputs[i]; (void)relative_path(input.path); (void)digest(input.digest);
            require(i == 0 || inputs[i - 1]->path != input.path, "Duplicate cache input path");
            require(input.bytes <= 64 * 1024 * 1024 && input.bytes <= 128 * 1024 * 1024 - total, "Cache input byte budget");
            total += input.bytes;
            if (std::string_view{input.path} == d.source) { source = true; require(input.bytes <= 16 * 1024 * 1024, "Cache source budget"); }
        }
        require(source, "Missing cache source fingerprint");
        std::vector<const AssetOutput*> outputs;
        for (const auto& output : d.metadata.outputs) { outputs.push_back(&output); }
        std::sort(outputs.begin(), outputs.end(), [](auto a, auto b) { return a->key < b->key; });
        KeyWriter writer;
        writer.field(1, "DeckerAssetCacheKey"); writer.field(2, d.versions.algorithm);
        writer.field(3, d.versions.cache); writer.field(4, d.versions.artifact);
        writer.field(5, d.versions.implementation); writer.field(6, d.versions.contract);
        writer.field(7, d.versions.importer); writer.field(8, d.source);
        writer.field(9, std::bit_cast<std::uint64_t>(d.metadata.unit_scale));
        writer.field(10, outputs.size());
        for (const auto* output : outputs) {
            writer.field(11, output->key); writer.field(12, asset_kind_name(output->kind)); writer.field(13, output->id.to_string());
        }
        writer.field(14, inputs.size());
        for (const auto* input : inputs) {
            writer.field(15, input->path); writer.field(16, input->bytes); writer.field(17, input->digest);
        }
        return writer.finish();
    });
}
CacheDescriptor describe_cache(const CpuArtifact& artifact)
{
    CacheDescriptor d; d.source = std::string{artifact.source}; d.inputs = artifact.inputs;
    d.metadata.root_id = artifact.data.mesh.id; d.metadata.unit_scale = artifact.data.unit_scale;
    for (const auto& o : artifact.data.outputs) { d.metadata.outputs.push_back({o.key, o.id, o.kind}); }
    return d;
}
Json descriptor_json(const CacheDescriptor& d)
{
    auto inputs = Json::array();
    for (const auto& i : d.inputs) { inputs.push_back({{"path", std::string{i.path}}, {"bytes", i.bytes}, {"digest", std::string{i.digest}}}); }
    return {{"algorithm", d.versions.algorithm}, {"importer", d.versions.importer}, {"cache_version", d.versions.cache},
        {"artifact_version", d.versions.artifact}, {"implementation_version", d.versions.implementation}, {"contract_version", d.versions.contract},
        {"source", d.source}, {"meta", Json::parse(take(serialize_asset_meta(d.metadata)))}, {"inputs", std::move(inputs)}};
}
std::string entry_directory(std::string_view key, std::string_view build)
{ return std::string{cache_root} + "/entries/" + digest(key) + "/" + uuid(build); }
CacheIndex parse_cache_index(std::string_view text)
{
    require(text.size() <= cache_index_limit, "Cache index byte limit"); auto j = parse(text);
    fields(j, {"format", "version", "algorithm", "root_id", "source", "key", "build"});
    require(j["format"] == "DeckerAssetCacheCurrent" && integer(j["version"]) == 1
        && j["algorithm"] == "xxh3-128-v1", "Incompatible cache index", ErrorCode::not_supported);
    CacheIndex result{take(AssetId::parse(uuid(string(j["root_id"])))), string(j["source"]), digest(string(j["key"])), uuid(string(j["build"]))};
    (void)relative_path(result.source); return result;
}
std::string index_json(const CacheIndex& i)
{
    return Json{{"format", "DeckerAssetCacheCurrent"}, {"version", 1}, {"algorithm", "xxh3-128-v1"},
        {"root_id", i.root.to_string()}, {"source", i.source}, {"key", i.key}, {"build", i.build}}.dump(2) + '\n';
}
Result<CacheEntry> read_cache_entry(const ProjectPaths& paths, std::string_view directory, std::string_view key, std::string_view build)
{
    return attempt<CacheEntry>("read_cache_entry", [&] {
        const auto base = std::string{directory};
        const auto text = read_cache_text(persistent_path(paths, base + "/entry.json"), cache_entry_limit);
        const auto j = parse(text); fields(j, {"format", "version", "algorithm", "key", "build", "descriptor", "manifest_digest"});
        require(j["format"] == "DeckerAssetCacheEntry" && integer(j["version"]) == 1 && j["algorithm"] == "xxh3-128-v1",
            "Incompatible cache entry", ErrorCode::not_supported);
        require(digest(string(j["key"])) == key && uuid(string(j["build"])) == build, "Cache entry location mismatch");
        auto d = descriptor(j["descriptor"]);
        require(d.versions == CacheVersions{}, "Incompatible cache implementation/contract", ErrorCode::not_supported);
        require(take(cache_key(d)).hex() == key, "Cache descriptor key mismatch");
        const auto manifest = read_cache_text(persistent_path(paths, base + "/manifest.json"), cache_entry_limit);
        require(content_digest(std::as_bytes(std::span{manifest.data(), manifest.size()})).hex() == digest(string(j["manifest_digest"])), "Cache manifest digest mismatch");
        (void)persistent_path(paths, base + "/data.bin");
        auto artifact = take(load_cpu_artifact(persistent_path(paths, base)));
        require(take(cache_key(describe_cache(artifact))).hex() == key, "CPU artifact differs from cache descriptor");
        return CacheEntry{std::move(d), std::move(artifact), text};
    });
}
} // namespace dk::asset_detail
