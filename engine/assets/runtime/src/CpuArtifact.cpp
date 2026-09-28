#include "CpuArtifactInternal.hpp"
#include "AssetInternal.hpp"
#include "ContentDigest.hpp"
#include <dk/profiling/Profiler.hpp>
#include <bit>
#include <map>
#include <set>

namespace dk {
namespace {
using namespace asset_detail;
using Json = nlohmann::json;
constexpr std::size_t data_limit = 256 * 1024 * 1024, manifest_limit = 16 * 1024 * 1024;
Json id(const std::optional<AssetId>& value) { return value ? Json(value->to_string()) : Json(nullptr); }
template<class V> Json vector_json(const V& v) { auto a = Json::array(); for (Eigen::Index i = 0; i < v.size(); ++i) { a.push_back(v[i]); } return a; }
void word(ByteBuffer& bytes, std::uint32_t value)
{ require(bytes.size() <= data_limit - 4, "CPU artifact byte limit"); for (unsigned i = 0; i < 4; ++i) { bytes.push_back(static_cast<std::byte>((value >> (8 * i)) & 255)); } }
template<class V> Json vectors(ByteBuffer& bytes, const Vector<V>& values)
{
    const auto offset = bytes.size();
    for (const auto& value : values) { for (Eigen::Index i = 0; i < value.size(); ++i) { require(std::isfinite(value[i]), "Nonfinite CPU value"); word(bytes,std::bit_cast<std::uint32_t>(value[i])); } }
    return Json{{"offset",offset},{"count",values.size()}};
}
Json sampler(const TextureSampler& s)
{
    return {{"min_filter",s.min_filter ? Json(static_cast<unsigned>(*s.min_filter)) : Json(nullptr)},
        {"mag_filter",s.mag_filter ? Json(static_cast<unsigned>(*s.mag_filter)) : Json(nullptr)},
        {"wrap_s",static_cast<unsigned>(s.wrap_s)},{"wrap_t",static_cast<unsigned>(s.wrap_t)}};
}
void fields(const Json& value, std::initializer_list<std::string_view> names)
{
    require(value.is_object() && value.size() == names.size(), "Invalid CPU artifact fields");
    for (auto name : names) { require(value.contains(name), "Missing CPU artifact field: " + std::string{name}); }
}
std::string string(const Json& value) { require(value.is_string(),"Expected artifact string"); return value.get<std::string>(); }
std::size_t number(const Json& value, std::size_t max)
{ require(value.is_number_unsigned() && value.get<std::uint64_t>() <= max,"Invalid artifact integer or limit"); return value.get<std::size_t>(); }
float scalar(const Json& value)
{ require(value.is_number(),"Expected artifact number"); const double v = value.get<double>(); require(std::isfinite(v) && std::abs(v) <= std::numeric_limits<float>::max(),"Invalid artifact number"); return static_cast<float>(v); }
template<class V> V vector_value(const Json& value)
{
    V result; require(value.is_array() && value.size() == static_cast<std::size_t>(result.size()),"Invalid vector shape");
    for (Eigen::Index i = 0; i < result.size(); ++i) { result[i] = scalar(value[static_cast<std::size_t>(i)]); } return result;
}
std::string hash(const Json& value)
{
    auto s = string(value); require(s.size() == 32 && std::all_of(s.begin(),s.end(),[](char c){return (c>='0'&&c<='9')||(c>='a'&&c<='f');}),"Invalid digest"); return s;
}
void array(const Json& value, std::size_t limit) { require(value.is_array() && value.size() <= limit,"Invalid artifact array"); }
class Reader {
public:
    std::span<const std::byte> bytes;
    std::size_t cursor = 0, vertices = 0, indices = 0, textures = 0;
    std::map<AssetId,AssetKind> ids;
    std::set<AssetId> consumed;
    AssetId identity(const Json& value, AssetKind kind, bool consume = false) {
        const auto result = take(AssetId::parse(string(value)));
        require(ids.contains(result) && ids.at(result) == kind,"Artifact identity/kind differs from meta");
        if (consume) { require(consumed.insert(result).second,"Duplicate artifact identity"); } return result;
    }
    std::uint32_t word() {
        require(cursor <= bytes.size() && bytes.size() - cursor >= 4,"Truncated artifact data"); std::uint32_t value=0;
        for(unsigned i=0;i<4;++i){ value |= std::to_integer<std::uint32_t>(bytes[cursor++]) << (i*8); } return value;
    }
    std::size_t block(const Json& value, std::size_t width, std::size_t limit) {
        fields(value,{"offset","count"}); require(number(value["offset"],data_limit)==cursor,"Noncanonical artifact block offset");
        const auto count = number(value["count"],limit); require(cursor <= bytes.size() && count <= (bytes.size()-cursor)/width,"Artifact block out of bounds"); return count;
    }
    template<class V> Vector<V> vectors(const Json& value, std::size_t limit) {
        constexpr auto components = static_cast<std::size_t>(V::RowsAtCompileTime);
        const auto count = block(value,components*4,limit); Vector<V> result(count);
        for(auto& v:result){for(Eigen::Index i=0;i<v.size();++i){v[i]=std::bit_cast<float>(word()); require(std::isfinite(v[i]),"Nonfinite artifact vertex");}} return result;
    }
};
TextureSampler read_sampler(const Json& value)
{
    fields(value,{"min_filter","mag_filter","wrap_s","wrap_t"}); TextureSampler result;
    const auto filter = [&](std::string_view name,bool min)->std::optional<TextureFilter>{
        if(value[name].is_null()) return {};
        const auto v=number(value[name],9987); require(v==9728||v==9729||(min&&v>=9984&&v<=9987),"Invalid artifact filter");return static_cast<TextureFilter>(v);
    };
    const auto wrap=[&](std::string_view name){const auto v=number(value[name],33648);require(v==10497||v==33071||v==33648,"Invalid artifact wrap");return static_cast<TextureWrap>(v);};
    result.min_filter=filter("min_filter",true);result.mag_filter=filter("mag_filter",false);result.wrap_s=wrap("wrap_s");result.wrap_t=wrap("wrap_t");return result;
}
}
namespace asset_detail {
Result<EncodedCpuArtifact> encode_cpu_artifact(const ImportResult& imported, const AssetMetadata& metadata)
{
    return attempt<EncodedCpuArtifact>("encode_cpu_artifact",[&]{
        require(!imported.inputs.empty(),"Artifact needs source snapshot"); EncodedCpuArtifact result;
        auto primitives=Json::array(),materials=Json::array(),textures=Json::array(),inputs=Json::array(),diagnostics=Json::array();
        for(const auto& p:imported.mesh.primitives){
            Json j{{"positions",vectors(result.bytes,p.positions)},{"normals",vectors(result.bytes,p.normals)},{"texcoords",vectors(result.bytes,p.texcoords)}};
            const auto offset=result.bytes.size();for(auto index:p.indices){word(result.bytes,index);}j["indices"]={{"offset",offset},{"count",p.indices.size()}};
            j["bounds_min"]=vector_json(p.bounds_min);j["bounds_max"]=vector_json(p.bounds_max);j["material"]=id(p.material);primitives.push_back(std::move(j));
        }
        for(const auto& m:imported.materials){materials.push_back({{"id",m.id.to_string()},{"base_color",vector_json(m.base_color)},{"emissive",vector_json(m.emissive)},
            {"metallic",m.metallic},{"roughness",m.roughness},{"alpha_cutoff",m.alpha_cutoff},{"alpha_mode",static_cast<unsigned>(m.alpha_mode)},
            {"double_sided",m.double_sided},{"base_color_texture",id(m.base_color_texture)}});}
        for(const auto& t:imported.textures){
            const auto offset=result.bytes.size();require(t.rgba8.size()<=data_limit-result.bytes.size(),"Artifact data limit");result.bytes.insert(result.bytes.end(),t.rgba8.begin(),t.rgba8.end());
            textures.push_back({{"id",t.id.to_string()},{"width",t.width},{"height",t.height},{"rgba8",{{"offset",offset},{"count",t.rgba8.size()}}},
                {"origin",std::string{t.origin}},{"sampler",sampler(t.sampler)},{"color_space","srgb"},{"row_order","top-to-bottom"}});
        }
        for(const auto& input:imported.inputs){inputs.push_back({{"path",std::string{input.path}},{"bytes",input.bytes.size()},{"digest",content_digest(input.bytes).hex()}});}
        for(const auto& diagnostic:imported.diagnostics){diagnostics.push_back(std::string{diagnostic});}
        const auto meta=take(serialize_asset_meta(metadata));
        const Json manifest{{"format","DeckerCpuAsset"},{"version",1},{"hash_algorithm","xxh3-128-v1"},{"meta",Json::parse(meta)},
            {"source",std::string{imported.inputs.front().path}},{"inputs",std::move(inputs)},{"primitives",std::move(primitives)},
            {"materials",std::move(materials)},{"textures",std::move(textures)},{"diagnostics",std::move(diagnostics)},
            {"data",{{"file","data.bin"},{"bytes",result.bytes.size()},{"digest",content_digest(result.bytes).hex()}}}};
        result.manifest=manifest.dump(2)+'\n';require(result.manifest.size()<=manifest_limit,"Artifact manifest limit");return result;
    });
}
}
Result<CpuArtifact> load_cpu_artifact(const std::filesystem::path& directory)
{
    DK_PROFILE_ZONE("Assets.LoadCpuArtifact");
    return attempt<CpuArtifact>("load_cpu_artifact",[&]{
        const auto text=take(read_file_bytes(directory/"manifest.json",manifest_limit));std::vector<std::set<std::string>> keys;
        const auto j=Json::parse(text.begin(),text.end(),[&](int depth,Json::parse_event_t event,Json& value){
            require(depth<=64,"Artifact JSON nesting limit");if(event==Json::parse_event_t::object_start)keys.emplace_back();
            if(event==Json::parse_event_t::key)require(keys.back().insert(string(value)).second,"Duplicate artifact key");
            if(event==Json::parse_event_t::object_end)keys.pop_back();return true;
        });
        fields(j,{"format","version","hash_algorithm","meta","source","inputs","primitives","materials","textures","diagnostics","data"});
        require(j["format"]=="DeckerCpuAsset","Unknown CPU artifact format");require(j["version"].is_number_integer(),"Invalid artifact version");
        require(j["version"]==1&&j["hash_algorithm"]=="xxh3-128-v1","Unsupported artifact version or hash",ErrorCode::not_supported);
        const auto metadata=take(parse_asset_meta(j["meta"].dump()));fields(j["data"],{"file","bytes","digest"});require(j["data"]["file"]=="data.bin","Invalid artifact data file");
        const auto bytes=take(read_file_bytes(directory/"data.bin",data_limit));require(bytes.size()==number(j["data"]["bytes"],data_limit)&&content_digest(bytes).hex()==hash(j["data"]["digest"]),"CPU artifact digest/size mismatch");
        Reader reader{bytes};CpuArtifact result;result.source=owned(string(j["source"]));(void)relative_path(result.source);
        result.data.unit_scale=metadata.unit_scale;result.data.mesh.id=metadata.root_id;
        for(const auto& o:metadata.outputs){reader.ids.emplace(o.id,o.kind);result.data.outputs.push_back({o.key,o.id,o.kind});}reader.consumed.insert(metadata.root_id);
        array(j["inputs"],20001);require(!j["inputs"].empty(),"Missing artifact input fingerprints");std::set<std::string> paths;std::size_t input_bytes=0;
        for(const auto& input:j["inputs"]){fields(input,{"path","bytes","digest"});auto path=string(input["path"]);(void)relative_path(path);require(paths.insert(path).second,"Duplicate artifact input");
            const auto size=number(input["bytes"],64*1024*1024);require(size<=128*1024*1024-input_bytes,"Artifact input budget");input_bytes+=size;result.inputs.push_back({owned(path),owned(hash(input["digest"])),size});}
        require(result.inputs.front().path==result.source,"Artifact source fingerprint mismatch");
        require(result.inputs.front().bytes<=16*1024*1024,"Artifact source budget");
        array(j["primitives"],4096);require(!j["primitives"].empty(),"No artifact primitive");
        for(const auto& item:j["primitives"]){
            fields(item,{"positions","normals","texcoords","indices","bounds_min","bounds_max","material"});MeshPrimitive p;
            p.positions=reader.vectors<Vec3f>(item["positions"],1000000-reader.vertices);require(!p.positions.empty(),"No vertices");reader.vertices+=p.positions.size();
            p.normals=reader.vectors<Vec3f>(item["normals"],p.positions.size());p.texcoords=reader.vectors<Vec2f>(item["texcoords"],p.positions.size());
            require((p.normals.empty()||p.normals.size()==p.positions.size())&&(p.texcoords.empty()||p.texcoords.size()==p.positions.size()),"Artifact attribute count mismatch");
            const auto count=reader.block(item["indices"],4,3000000-reader.indices);require(count>0&&count%3==0,"Invalid artifact triangles");reader.indices+=count;p.indices.resize(count);
            for(auto& index:p.indices){index=reader.word();require(index<p.positions.size(),"Artifact index out of bounds");}
            p.bounds_min=p.bounds_max=p.positions[0];for(const auto& v:p.positions){p.bounds_min=p.bounds_min.cwiseMin(v);p.bounds_max=p.bounds_max.cwiseMax(v);}
            require(p.bounds_min==vector_value<Vec3f>(item["bounds_min"])&&p.bounds_max==vector_value<Vec3f>(item["bounds_max"]),"Artifact bounds mismatch");
            if(!item["material"].is_null())p.material=reader.identity(item["material"],AssetKind::material);result.data.mesh.primitives.push_back(std::move(p));
        }
        array(j["materials"],10000);
        for(const auto& item:j["materials"]){fields(item,{"id","base_color","emissive","metallic","roughness","alpha_cutoff","alpha_mode","double_sided","base_color_texture"});
            MaterialData m;m.id=reader.identity(item["id"],AssetKind::material,true);m.base_color=vector_value<Vec4f>(item["base_color"]);m.emissive=vector_value<Vec3f>(item["emissive"]);
            m.metallic=scalar(item["metallic"]);m.roughness=scalar(item["roughness"]);m.alpha_cutoff=scalar(item["alpha_cutoff"]);m.alpha_mode=static_cast<AlphaMode>(number(item["alpha_mode"],2));
            require(item["double_sided"].is_boolean(),"Invalid double_sided");m.double_sided=item["double_sided"].get<bool>();
            require((m.base_color.array()>=0).all()&&(m.base_color.array()<=1).all()&&(m.emissive.array()>=0).all()&&(m.emissive.array()<=1).all()
                &&m.metallic>=0&&m.metallic<=1&&m.roughness>=0&&m.roughness<=1&&m.alpha_cutoff>=0,"Invalid material factors");
            if(!item["base_color_texture"].is_null())m.base_color_texture=reader.identity(item["base_color_texture"],AssetKind::texture);result.data.materials.push_back(std::move(m));
        }
        array(j["textures"],10000);
        for(const auto& item:j["textures"]){fields(item,{"id","width","height","rgba8","origin","sampler","color_space","row_order"});TextureData t;
            require(item["color_space"]=="srgb"&&item["row_order"]=="top-to-bottom","Unsupported texture representation",ErrorCode::not_supported);
            t.id=reader.identity(item["id"],AssetKind::texture,true);t.width=static_cast<std::uint32_t>(number(item["width"],8192));t.height=static_cast<std::uint32_t>(number(item["height"],8192));
            require(t.width>0&&t.height>0,"Invalid image size");const auto count=reader.block(item["rgba8"],1,64*1024*1024);
            require(count==static_cast<std::size_t>(t.width)*t.height*4&&count<=128*1024*1024-reader.textures,"Invalid image byte count");reader.textures+=count;
            t.rgba8.assign(bytes.begin()+static_cast<std::ptrdiff_t>(reader.cursor),bytes.begin()+static_cast<std::ptrdiff_t>(reader.cursor+count));reader.cursor+=count;
            t.origin=owned(string(item["origin"]));require(t.origin.size()<=8192,"Origin too long");t.sampler=read_sampler(item["sampler"]);result.data.textures.push_back(std::move(t));
        }
        require(reader.cursor==bytes.size()&&reader.consumed.size()==reader.ids.size(),"Unused data or missing output identity");
        for(const auto& p:result.data.mesh.primitives){if(p.material){const auto m=std::find_if(result.data.materials.begin(),result.data.materials.end(),[&](const auto& v){return v.id==*p.material;});require(m!=result.data.materials.end()&&(!m->base_color_texture||!p.texcoords.empty()),"Invalid material/UV binding");}}
        array(j["diagnostics"],10000);for(const auto& d:j["diagnostics"]){auto value=string(d);require(value.size()<=8192,"Diagnostic too long");result.data.diagnostics.push_back(owned(value));}
        return result;
    });
}
} // namespace dk
