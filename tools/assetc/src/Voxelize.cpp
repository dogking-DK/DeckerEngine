#include <dk/assets/MeshVoxelizer.hpp>
#include <dk/assets/GltfImporter.hpp>
#include <dk/assets/AssetCache.hpp>
#include <dk/geometry/SparseVoxels.hpp>
#include <nlohmann/json.hpp>
#include <charconv>
#include <chrono>
#include <iostream>
#include <map>
#include <thread>

namespace {
using Json=nlohmann::json;
template<class T> T take(dk::Result<T> r) { if(!r) throw r.error(); return std::move(*r); }
void save(const std::filesystem::path& p,std::string_view text) {
    const auto r=dk::write_file_bytes(p,std::as_bytes(std::span{text.data(),text.size()})); if(!r) throw r.error();
}
}
int voxelize(const dk::ProjectPaths& paths,const std::map<std::string,std::string>& flags,double scale) {
    std::filesystem::path staging;
    try {
        dk::assets::MeshVoxelizeOptions options;
        if(flags.contains("--resolution")) {
            const auto& s=flags.at("--resolution"); auto p=std::from_chars(s.data(),s.data()+s.size(),options.resolution);
            if(p.ec!=std::errc{} || p.ptr!=s.data()+s.size() || options.resolution<16 || options.resolution>512) throw dk::Error{dk::ErrorCode::invalid_argument,"Resolution must be 16..512"};
        }
        if(flags.contains("--colors")) {
            const auto& s=flags.at("--colors");
            if(s!="texture" && s!="material") throw dk::Error{dk::ErrorCode::invalid_argument,"Colors must be texture or material"};
            options.texture_colors=s=="texture";
        }
        const auto output=take(paths.resolve(take(dk::path_from_utf8(flags.at("--output")))));
        if(output==paths.root() || std::filesystem::exists(output)) throw dk::Error{dk::ErrorCode::conflict,"Voxel output must be a new directory"};
        if(!std::filesystem::is_directory(output.parent_path())) throw dk::Error{dk::ErrorCode::not_found,"Voxel output parent directory must exist"};
        const auto start=std::chrono::steady_clock::now();
        auto source=take(dk::import_gltf(paths,{.source=flags.at("--source"),.unit_scale=scale,.profile=dk::GltfImportProfile::unlit_preview}));
        const auto import_ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
        auto converted=take(dk::assets::voxelize_mesh(source,options));
        // Keep the sibling name short: cache descendants already contain content hashes.
        const auto candidate=output.parent_path()/(".vxl-"+take(dk::AssetId::generate()).to_string().substr(0,18));
        if(!std::filesystem::create_directory(candidate)) throw dk::Error{dk::ErrorCode::io_error,"Cannot create voxel staging directory"};
        staging=candidate; // Cleanup owns only a directory this invocation actually created.
        if(auto r=dk::write_file_bytes(staging/"grid.vdb",converted.vdb);!r) throw r.error();
        save(staging/"solid.dkvoxel",converted.description_json);
        auto bundle_paths=take(dk::ProjectPaths::create(staging));
        const auto compile_start=std::chrono::steady_clock::now();
        const auto compiled=take(dk::compile_cached_asset(bundle_paths,{"solid.dkvoxel"}));
        const auto compile_ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-compile_start).count();
        const auto id=dk::root_asset_id(compiled.artifact.data).to_string();
        const auto entity=take(dk::AssetId::generate()).to_string(),scene=take(dk::AssetId::generate()).to_string();
        Json project={{"format","DeckerProject"},{"version",1},{"name","Voxelized mesh"},{"scene","scene.json"},
            {"assets",Json::array({{{"id",id},{"kind","voxel"},{"path","solid.dkvoxel"}}})}};
        Json components={{"dk.Identity",{{"version",1},{"id",entity}}},{"dk.Name",{{"version",1},{"value","Voxelized mesh"}}},
            {"dk.Transform",{{"version",1},{"translation",{0,0,0}},{"rotation",{0,0,0,1}},{"scale",{1,1,1}}}},
            {"dk.Hierarchy",{{"version",1},{"parent",nullptr}}},
            {"dk.AssetReferences",{{"version",1},{"items",Json::array({{{"id",id},{"kind","voxel"}}})}}}};
        save(staging/"project.json",project.dump(2));
        save(staging/"scene.json",Json{{"format","DeckerScene"},{"version",1},{"scene_id",scene},{"revision",1},
            {"entities",Json::array({{{"components",components}}})}}.dump(2));
        auto stats=Json::parse(converted.statistics_json);
        stats["source"]=flags.at("--source"); stats["unit_scale"]=scale; stats["import_ms"]=import_ms; stats["compile_nano_ms"]=compile_ms;
        stats["nano_bytes"]=compiled.artifact.data.voxel->grid->statistics().nano_bytes;
        stats["elapsed_ms"]=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
        stats["diagnostics"]=Json::array(); for(const auto& d:source.diagnostics) stats["diagnostics"].push_back(std::string(d));
        save(staging/"conversion.json",stats.dump(2)+'\n');
        // Final publication: no replacement of existing user output.
        std::error_code publish_error;
        for(unsigned attempt=0;;++attempt) {
            std::filesystem::rename(staging,output,publish_error);
            if(!publish_error) break;
            if(publish_error!=std::errc::permission_denied || attempt==20 || std::filesystem::exists(output))
                throw std::filesystem::filesystem_error("Publish voxel bundle",staging,output,publish_error);
            // Windows scanners may briefly retain newly closed cache files.
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        }
        staging.clear();
        stats["output"]=flags.at("--output"); std::cout<<stats.dump(2)<<'\n'; return 0;
    } catch(const dk::Error& error) {
        std::cerr<<dk::error_code_name(error.code)<<": "<<error.message;
        for(const auto& context:error.context) std::cerr<<" ["<<context<<"]";
        std::cerr<<'\n';
    } catch(const std::exception& error) { std::cerr<<"Voxel export failed: "<<error.what()<<'\n'; }
    if(!staging.empty()) { std::error_code ec; std::filesystem::remove_all(staging,ec); if(ec) std::cerr<<"Unpublished staging remains: "<<staging<<'\n'; }
    return 1;
}
