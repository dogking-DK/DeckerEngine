#include "CpuArtifactInternal.hpp"
#include "AssetInternal.hpp"
#include "ContentDigest.hpp"
#include <dk/assets/VoxelImporter.hpp>
#include <set>

namespace dk::asset_detail {
namespace {
using Json=nlohmann::json;
void fields(const Json& j,std::initializer_list<std::string_view> names) {
    require(j.is_object() && j.size()==names.size(),"Unexpected voxel artifact fields");
    for(auto n:names) require(j.contains(n),"Missing voxel artifact field");
}
std::string text(const Json& j) { require(j.is_string(),"Expected voxel artifact string"); return j.get<std::string>(); }
std::uint64_t number(const Json& j,std::uint64_t max) { require(j.is_number_unsigned() && j.get<std::uint64_t>()<=max,"Invalid voxel artifact integer"); return j.get<std::uint64_t>(); }
std::string digest(const Json& j) {
    auto s=text(j); require(s.size()==32 && std::all_of(s.begin(),s.end(),[](char c){return (c>='0' && c<='9') || (c>='a' && c<='f');}),"Invalid voxel artifact digest"); return s;
}
Json palette_json(const Vector<VoxelMaterialData>& palette) {
    require(!palette.empty() && palette.size()<=65536,"Voxel palette count limit"); auto j=Json::array();
    for(const auto& m:palette) {
        require(m.base_color.allFinite() && (m.base_color.array()>=0).all() && (m.base_color.array()<=1).all() &&
            std::isfinite(m.metallic) && m.metallic>=0 && m.metallic<=1 && std::isfinite(m.roughness) && m.roughness>=0 && m.roughness<=1,"Invalid voxel material");
        j.push_back({{"base_color",{m.base_color.x(),m.base_color.y(),m.base_color.z()}},{"metallic",m.metallic},{"roughness",m.roughness}});
    } return j;
}
float factor(const Json& j) { require(j.is_number(),"Expected voxel factor"); const auto f=j.get<double>(); require(std::isfinite(f) && f>=0 && f<=1,"Invalid voxel factor"); return float(f); }
geometry::voxels::Coord coordinate(const Json& j) {
    require(j.is_array() && j.size()==3,"Invalid voxel coordinate"); geometry::voxels::Coord c;
    for(int i=0;i<3;++i) { require(j[i].is_number_integer() && (!j[i].is_number_unsigned() || j[i].get<std::uint64_t>()<=INT64_MAX),"Expected signed-range integer coordinate"); auto n=j[i].get<std::int64_t>(); require(n>=-geometry::voxels::coordinate_limit && n<=geometry::voxels::coordinate_limit,"Voxel coordinate range"); c[i]=static_cast<std::int32_t>(n); }
    return c;
}
}
Result<EncodedCpuArtifact> encode_voxel_artifact(const ImportResult& input,const AssetMetadata& meta) {
    return attempt<EncodedCpuArtifact>("encode_voxel_artifact",[&] {
        validate_meta(meta); require(metadata_importer(meta)=="voxel-vdb" && input.voxel && input.voxel->grid && input.voxel->id==meta.root_id &&
            input.mesh.id.is_nil() && input.mesh.primitives.empty() && input.materials.empty() && input.textures.empty(),"Invalid voxel asset payload");
        require(input.inputs.size()==2 && input.inputs[0].path.ends_with(".dkvoxel") && input.inputs[1].path.ends_with(".vdb"),"Voxel artifact needs descriptor and VDB input");
        const auto& data=*input.voxel; const auto& grid=*data.grid; auto palette=palette_json(data.palette);
        require(grid.maximum_value()<=data.palette.size(),"Voxel palette omits values");
        auto matrix=Json::array(); for(int r=0;r<4;++r) for(int c=0;c<4;++c) matrix.push_back(grid.corner_to_local().matrix()(r,c));
        EncodedCpuArtifact result; const auto bytes=grid.bytes(); require(bytes.size()<=256*1024*1024,"Voxel artifact byte limit"); result.bytes.assign(bytes.begin(),bytes.end());
        auto inputs=Json::array(); for(const auto& i:input.inputs) inputs.push_back({{"path",std::string{i.path}},{"bytes",i.bytes.size()},{"digest",content_digest(i.bytes).hex()}});
        const auto& d=grid.domain();
        Json j{{"format","DeckerVoxelAsset"},{"version",1},{"hash_algorithm","xxh3-128-v1"},{"meta",Json::parse(take(serialize_asset_meta(meta)))},
            {"source",std::string{input.inputs[0].path}},{"inputs",inputs},{"semantics","solid-material-index"},{"coordinates","cell-corner"},
            {"domain",{{"minimum",d.minimum},{"maximum",d.maximum}}},{"corner_to_local",matrix},{"palette",palette},
            {"layout",{{"name","NanoVDB-Int32"},{"version",geometry::voxels::sparse_layout_version()},{"endian","little"},{"builder",assets::voxel_builder_signature()}}},
            {"data",{{"file","data.bin"},{"bytes",bytes.size()},{"digest",content_digest(bytes).hex()}}}};
        result.manifest=j.dump(2)+'\n'; require(result.manifest.size()<=16*1024*1024,"Voxel manifest byte limit"); return result;
    });
}
Result<CpuArtifact> load_voxel_artifact(const std::filesystem::path& directory,std::string_view manifest) {
    return attempt<CpuArtifact>("load_voxel_artifact",[&] {
        // Outer dispatcher already enforced duplicate keys, depth and the manifest byte budget.
        const auto j=Json::parse(manifest);
        fields(j,{"format","version","hash_algorithm","meta","source","inputs","semantics","coordinates","domain","corner_to_local","palette","layout","data"});
        require(j["format"]=="DeckerVoxelAsset" && j["version"].is_number_integer() && j["version"]==1 && j["hash_algorithm"]=="xxh3-128-v1","Unsupported voxel artifact version",ErrorCode::not_supported);
        require(j["semantics"]=="solid-material-index" && j["coordinates"]=="cell-corner","Unsupported voxel artifact semantics",ErrorCode::not_supported);
        const auto& layout=j["layout"]; fields(layout,{"name","version","endian","builder"});
        require(layout["name"]=="NanoVDB-Int32" && layout["endian"]=="little" && layout["version"].is_number_unsigned() &&
            layout["version"]==geometry::voxels::sparse_layout_version() && text(layout["builder"])==assets::voxel_builder_signature(),"Incompatible voxel builder/layout",ErrorCode::not_supported);
        const auto meta=take(parse_asset_meta(j["meta"].dump())); require(metadata_importer(meta)=="voxel-vdb","Voxel artifact metadata mismatch");
        CpuArtifact result; result.source=owned(text(j["source"])); (void)relative_path(result.source);
        require(result.source.ends_with(".dkvoxel"),"Voxel artifact source extension");
        result.data.unit_scale=meta.unit_scale; for(const auto& o:meta.outputs) result.data.outputs.push_back({o.key,o.id,o.kind});
        const auto& inputs=j["inputs"]; require(inputs.is_array() && inputs.size()==2,"Voxel artifact needs both input fingerprints");
        for(unsigned i=0;i<2;++i) {
            fields(inputs[i],{"path","bytes","digest"}); auto path=text(inputs[i]["path"]); (void)relative_path(path);
            if(i==0) require(path==std::string_view{result.source},"Voxel descriptor fingerprint differs"); else require(path.ends_with(".vdb"),"Voxel dependency extension");
            result.inputs.push_back({owned(path),owned(digest(inputs[i]["digest"])),number(inputs[i]["bytes"],i ? 64*1024*1024 : 2*1024*1024)});
        }
        fields(j["domain"],{"minimum","maximum"}); const geometry::voxels::Domain domain{coordinate(j["domain"]["minimum"]),coordinate(j["domain"]["maximum"])};
        const auto& values=j["corner_to_local"]; require(values.is_array() && values.size()==16,"Invalid voxel affine matrix"); Mat4d matrix;
        for(int r=0;r<4;++r) for(int c=0;c<4;++c) { require(values[r*4+c].is_number(),"Expected transform number"); matrix(r,c)=values[r*4+c].get<double>(); }
        const auto transform=take(Transformd::from_matrix(matrix));
        VoxelData data; data.id=meta.root_id; const auto& palette=j["palette"];
        require(palette.is_array() && !palette.empty() && palette.size()<=65536,"Voxel palette count limit");
        for(const auto& p:palette) {
            fields(p,{"base_color","metallic","roughness"}); require(p["base_color"].is_array() && p["base_color"].size()==3,"Invalid voxel RGB"); VoxelMaterialData m;
            for(int a=0;a<3;++a) m.base_color[a]=factor(p["base_color"][a]); m.metallic=factor(p["metallic"]); m.roughness=factor(p["roughness"]); data.palette.push_back(m);
        }
        fields(j["data"],{"file","bytes","digest"}); require(j["data"]["file"]=="data.bin","Invalid voxel data file");
        const auto size=number(j["data"]["bytes"],256*1024*1024); const auto bytes=take(read_file_bytes(directory/"data.bin",static_cast<std::size_t>(size)));
        require(bytes.size()==size && content_digest(bytes).hex()==digest(j["data"]["digest"]),"Voxel payload digest/size mismatch");
        auto grid=take(geometry::voxels::load_sparse_grid(bytes,domain,transform,static_cast<std::uint32_t>(data.palette.size())));
        data.grid=std::make_shared<const geometry::voxels::SparseGrid>(std::move(grid)); result.data.voxel=std::move(data); return result;
    });
}
} // namespace dk::asset_detail
