#include <dk/assets/VoxelImporter.hpp>
#include <dk/io/File.hpp>
#include <dk/memory/Context.hpp>
#include "OpenVdbConversion.hpp"
#include <openvdb/io/Stream.h>
#include <nlohmann/json.hpp>
#include <spanstream>
#include <set>
#include <cmath>

namespace dk::assets {
namespace {
using Json=nlohmann::json;
void require(bool b,const char* message,ErrorCode code=ErrorCode::invalid_argument) { if(!b) throw Error{code,message}; }
template<class T> T take(Result<T> r) { if(!r) throw r.error(); return std::move(*r); }
String owned(std::string_view s) { return {s.begin(),s.end()}; }
Vector<std::byte> read_owned(const std::filesystem::path& path,std::size_t limit) {
    const auto bytes=take(read_file_bytes(path,limit)); return {bytes.begin(),bytes.end()};
}
void fields(const Json& j,std::initializer_list<std::string_view> keys) {
    require(j.is_object() && j.size()==keys.size(),"Unexpected voxel source fields");
    for(auto key:keys) require(j.contains(key),"Missing voxel source field");
}
std::string text(const Json& j) { require(j.is_string(),"Expected voxel source string"); return j.get<std::string>(); }
std::filesystem::path relative(std::string_view s) {
    require(!s.empty() && s.size()<=4096 && s.find_first_of("\\:")==s.npos && s.find('\0')==s.npos,"Invalid voxel relative path");
    auto p=take(path_from_utf8(s)); require(!p.has_root_path(),"Voxel path must be relative"); return p;
}
geometry::voxels::Coord coordinate(const Json& j) {
    require(j.is_array() && j.size()==3,"Invalid voxel domain coordinate"); geometry::voxels::Coord c;
    for(int i=0;i<3;++i) {
        require(j[i].is_number_integer() && (!j[i].is_number_unsigned() || j[i].get<std::uint64_t>()<=INT64_MAX),"Voxel coordinate must be signed-range integer");
        const auto n=j[i].get<std::int64_t>(); require(n>=-geometry::voxels::coordinate_limit && n<=geometry::voxels::coordinate_limit,"Voxel coordinate out of range"); c[i]=static_cast<std::int32_t>(n);
    }
    return c;
}
float factor(const Json& j) { require(j.is_number(),"Expected voxel material number"); const auto n=j.get<double>(); require(std::isfinite(n) && n>=0 && n<=1,"Voxel material factor outside [0,1]"); return float(n); }
}
std::string_view voxel_builder_signature() noexcept { return DK_VOXEL_BUILDER_SIGNATURE; }
Result<ImportResult> import_voxel(const ProjectPaths& paths,const VoxelImportRequest& request) {
    try {
        require(memory::current_resource().state()==memory::ResourceState::open,"Voxel import memory is closing",ErrorCode::invalid_state);
        auto checkpoint=[&] { require(!request.stop.stop_requested(),"Voxel import cancelled",ErrorCode::invalid_state); }; checkpoint();
        const auto& limits=request.limits;
        require(limits.description_bytes && limits.description_bytes<=2*1024*1024 && limits.file_bytes && limits.file_bytes<=64*1024*1024 &&
            limits.input_bytes && limits.input_bytes<=128*1024*1024 && limits.decoded_tree_bytes && limits.live_payload_bytes && limits.build.maximum_tree_bytes && limits.build.maximum_nano_bytes,"Invalid voxel file budgets");
        require(std::isfinite(request.unit_scale) && request.unit_scale>0,"Invalid voxel unit scale");
        const auto source=relative(request.source).lexically_normal(); const auto source_name=take(path_to_utf8(source));
        require(source_name==request.source && source.extension()==".dkvoxel","Voxel source must be a canonical .dkvoxel descriptor");
        ImportResult result; result.unit_scale=request.unit_scale;
        auto description=read_owned(take(paths.resolve(source)),static_cast<std::size_t>(std::min(limits.description_bytes,limits.input_bytes)));
        std::vector<std::set<std::string>> keys;
        const auto j=Json::parse(description.begin(),description.end(),[&](int depth,Json::parse_event_t event,Json& value) {
            require(depth<=16,"Voxel source nesting limit");
            if(event==Json::parse_event_t::object_start) keys.emplace_back();
            if(event==Json::parse_event_t::key) require(keys.back().insert(text(value)).second,"Duplicate voxel source key");
            if(event==Json::parse_event_t::object_end) keys.pop_back(); return true;
        });
        fields(j,{"format","version","vdb","grid","semantics","transform","domain","palette"});
        require(j["format"]=="DeckerVoxelSource" && j["version"].is_number_integer() && j["version"]==1,"Unsupported voxel source format/version",ErrorCode::not_supported);
        require(j["semantics"]=="solid-material-index" && j["transform"]=="vdb-sample-center","Voxel semantics/transform must be explicit",ErrorCode::not_supported);
        const auto grid_name=text(j["grid"]); require(!grid_name.empty() && grid_name.size()<=255 && grid_name.find('\0')==grid_name.npos,"Invalid named voxel grid");
        fields(j["domain"],{"minimum","maximum"});
        VoxelBuildOptions options; options.domain=geometry::voxels::Domain{coordinate(j["domain"]["minimum"]),coordinate(j["domain"]["maximum"])};
        if(auto valid=geometry::voxels::validate(*options.domain); !valid) throw valid.error();
        require(j["palette"].is_array() && !j["palette"].empty() && j["palette"].size()<=65536,"Invalid voxel palette count");
        VoxelData data; options.palette_size=static_cast<std::uint32_t>(j["palette"].size()); options.limits=limits.build;
        for(const auto& item:j["palette"]) {
            fields(item,{"base_color","metallic","roughness"}); VoxelMaterialData m;
            require(item["base_color"].is_array() && item["base_color"].size()==3,"Voxel base color requires RGB");
            for(int i=0;i<3;++i) m.base_color[i]=factor(item["base_color"][i]);
            m.metallic=factor(item["metallic"]); m.roughness=factor(item["roughness"]); data.palette.push_back(m);
        }
        require(request.identities.size()<=1,"Voxel importer accepts only voxel/0");
        if(!request.identities.empty()) {
            const auto& identity=request.identities[0]; require(identity.key=="voxel/0" && identity.kind==AssetKind::voxel && !identity.id.is_nil(),"Voxel output identity differs from voxel/0"); data.id=identity.id;
        } else data.id=take(AssetId::generate());
        const auto dependency=(source.parent_path()/relative(text(j["vdb"]))).lexically_normal();
        require(dependency.extension()==".vdb","Voxel dependency must be .vdb"); const auto dependency_path=take(paths.resolve(dependency));
        const auto dependency_name=take(path_to_utf8(dependency));
        require(description.size()<limits.input_bytes,"No input budget for VDB dependency");
        auto bytes=read_owned(dependency_path,static_cast<std::size_t>(std::min(limits.file_bytes,limits.input_bytes-description.size()))); checkpoint();
        // Decode the same owned bytes that become dependency fingerprints; no delayed reads.
        openvdb::initialize();
        std::ispanstream input(std::span<const char>{reinterpret_cast<const char*>(bytes.data()),bytes.size()});
        openvdb::io::Stream archive(input,false); const auto grids=archive.getGrids();
        require(grids && grids->size()<=64,"VDB grid count exceeds 64");
        require(input.rdbuf()->in_avail()==0,"Unexpected trailing VDB bytes");
        std::uint64_t decoded=0; openvdb::GridBase::Ptr selected;
        for(const auto& grid:*grids) {
            const auto size=grid->memUsage(); require(size<=limits.decoded_tree_bytes-decoded,"Decoded VDB trees exceed budget"); decoded+=size;
            if(grid->getName()==grid_name) { require(!selected,"Ambiguous VDB grid name"); selected=grid; }
        }
        require(bool(selected),"Named VDB grid not found",ErrorCode::not_found); checkpoint();
        // Copy only the transform; the converter owns its separate normalized tree.
        require(selected->transform().isLinear(),"Nonlinear VDB transform",ErrorCode::not_supported);
        auto map=selected->transform().copy(); map->postScale(request.unit_scale); selected->setTransform(map);
        const auto source_bytes=selected->memUsage();
        const auto retained=std::uint64_t(description.size())+bytes.size()+decoded;
        require(retained<=limits.live_payload_bytes && source_bytes<=limits.live_payload_bytes-retained,"Voxel conversion live payload budget");
        options.limits.maximum_nano_bytes=std::min(options.limits.maximum_nano_bytes,limits.live_payload_bytes-retained-source_bytes);
        require(options.limits.maximum_nano_bytes>0,"No payload budget for NanoVDB conversion");
        auto converted=take(detail::convert_openvdb(*selected,options)); checkpoint();
        const auto payload=retained+converted.statistics().normalized_tree_bytes+converted.bytes().size();
        require(payload<=limits.live_payload_bytes,"Voxel conversion payload exceeded budget");
        data.grid=std::make_shared<const geometry::voxels::SparseGrid>(std::move(converted));
        result.outputs.push_back({"voxel/0",data.id,AssetKind::voxel}); result.voxel=std::move(data);
        result.inputs.push_back({owned(source_name),std::move(description)}); result.inputs.push_back({owned(dependency_name),std::move(bytes)});
        result.diagnostics.push_back(owned("VDB decoded_tree_bytes="+std::to_string(decoded)+" conversion_live_payload_bytes="+std::to_string(payload)+"; OpenVDB internal scratch/RSS not bounded"));
        return result;
    } catch(const Error& error) { return std::unexpected(error.with_context("import_voxel")); }
      catch(const nlohmann::json::exception& e) { return std::unexpected(Error{ErrorCode::invalid_argument,e.what(),{"voxel source JSON"}}); }
      catch(const std::bad_alloc&) { return std::unexpected(Error{ErrorCode::internal_error,"Voxel file conversion allocation failed"}); }
      catch(const std::exception& e) { return std::unexpected(Error{ErrorCode::invalid_argument,e.what(),{"OpenVDB file decode"}}); }
}
} // namespace dk::assets
