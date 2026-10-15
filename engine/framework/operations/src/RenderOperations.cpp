#include <dk/operations/RenderOperations.hpp>
#include <dk/operations/SceneOperations.hpp>
#include "EnvironmentJson.hpp"
namespace dk {
namespace {
Json integer(std::uint64_t maximum=UINT64_MAX,std::uint64_t minimum=0) {
    auto s=schema::integer(); s["minimum"]=minimum; s["maximum"]=maximum; return s;
}
Json uuid() { return schema::string(36,36); }
Json number(double low,double high) { return {{"type","number"},{"minimum",low},{"maximum",high}}; }
Json vector_schema() { return schema::array(schema::number(),3,3); }
Json path_schema(bool complete=false) {
    return schema::object({{"version",integer(1,1)},{"target_spp",integer(65536,1)},{"seed",integer()},
        {"max_bounces",integer(16,1)},{"rr_start",integer(16)},
        {"environment_rgb",schema::array(number(0,1e6),3,3)},{"sun",schema::boolean()},
        {"surface_only",schema::boolean()},{"exposure_ev",number(-32,32)}},
        complete ? Json{"version","target_spp","seed","max_bounces","rr_start","environment_rgb","sun","surface_only","exposure_ev"} : Json{"version","target_spp"});
}
Json camera_schema(bool complete=false) {
    const auto vec=vector_schema();
    return schema::object({{"eye",vec},{"target",vec},{"up",vec},{"fov_y",schema::number()},
        {"near",schema::number()},{"far",schema::number()}},
        complete ? Json{"eye","target","up","fov_y","near","far"} : Json{"eye","target"});
}
Json render_options_schema(bool complete=false) {
    return schema::object({{"mode",{{"type","string"},{"enum",{"reference","temporal"}}}},
        {"cloud_shadows",schema::boolean()},{"shadow_center_m",vector_schema()},
        {"shadow_extent_m",number(16,65536)},{"shadow_resolution",integer(512,16)}},
        complete ? Json{"mode","cloud_shadows","shadow_center_m","shadow_extent_m","shadow_resolution"} : Json::array());
}
Json provenance_schema() {
    return schema::object({{"environment",operations_detail::environment_schema()},{"camera",camera_schema(true)},
        {"exposure",schema::number()},{"time_seconds",schema::number()},
        {"quality",{{"type","string"},{"enum",{"atmosphere_v1","unlit_v1","cloud_reference_v1","cloud_temporal_v1","pathtrace_v1"}}}},
        {"renderer",{{"type","string"},{"enum",{"raster","pathtrace"}}}},{"format",{{"type","string"},{"enum",{"ppm","linear_json","sequence_json"}}}},
        {"pathtrace",schema::nullable(path_schema(true))},{"model_version",integer()},{"sampler_version",integer()},
        {"material_model",schema::string()},{"estimator",schema::string()},{"actual_spp",integer(65536)},
        {"input_fingerprint",{{"type",{"string","null"}}}},
        {"input_fingerprint_version",integer(2,2)},
        {"render_options",render_options_schema(true)},{"warmup_frames",integer(64)},{"sample_count",integer(128,1)},
        {"profile",{{"type","string"},{"enum",{"strict","unlit_preview"}}}},
        {"shader_fingerprint",{{"type",{"string","null"}}}}},
        {"environment","camera","exposure","time_seconds","quality","render_options","warmup_frames","sample_count","profile","shader_fingerprint"});
}
Json info_properties() { return {{"document_id",uuid()},{"scene_id",uuid()},{"revision",integer()},
    {"frame",integer(UINT64_MAX,1)},{"width",integer(2048,1)},{"height",integer(2048,1)},{"output",schema::string(1,1024)},{"provenance",provenance_schema()}}; }
Vec3d vector(const Json& value) { return {value[0].get<double>(),value[1].get<double>(),value[2].get<double>()}; }
CaptureCamera camera(const Json& c) {
    CaptureCamera value; value.eye=vector(c["eye"]); value.target=vector(c["target"]);
    if(c.contains("up")) value.up=vector(c["up"]);
    value.fov_y=c.value("fov_y",65.0); value.near_plane=c.value("near",.05); value.far_plane=c.value("far",100.0); return value;
}
CaptureRequest request(const Json& p,bool sequence) {
    CaptureRequest r; r.output=p["output"].get<std::string>(); r.width=p.value("width",960U); r.height=p.value("height",540U);
    r.profile=p.value("profile",std::string{"unlit_preview"})=="strict" ? GltfImportProfile::strict : GltfImportProfile::unlit_preview;
    r.exposure=p.value("exposure",1.f); r.require_validation=p.value("validation",std::string{"if_available"})=="required";
    r.warmup_frames=p.value("warmup_frames",0U); r.environment.time_seconds=p.value("time_seconds",0.0);
    r.pathtrace=p.value("renderer",std::string{"raster"})=="pathtrace";
    r.linear_json=p.value("format",std::string{"ppm"})=="linear_json";
    if(p.contains("pathtrace")) {
        const auto& v=p["pathtrace"]; CapturePathTrace path;
        path.version=v["version"].get<std::uint32_t>(); path.target_spp=v["target_spp"].get<std::uint32_t>();
        path.seed=v.value("seed",std::uint64_t{1}); path.max_bounces=v.value("max_bounces",8U); path.rr_start=v.value("rr_start",3U);
        if(v.contains("environment_rgb")) path.environment_rgb=vector(v["environment_rgb"]);
        path.sun=v.value("sun",false); path.surface_only=v.value("surface_only",false); path.exposure_ev=v.value("exposure_ev",0.0); r.path=path;
    }
    if(p.contains("render_options")) {
        const auto& o=p["render_options"]; auto& s=r.environment;
        s.mode=o.value("mode",std::string{"reference"})=="temporal" ? render::CloudRenderMode::temporal : render::CloudRenderMode::reference;
        s.cloud_shadows=o.value("cloud_shadows",true); s.shadow_extent_m=o.value("shadow_extent_m",8192.0); s.shadow_resolution=o.value("shadow_resolution",256U);
        if(o.contains("shadow_center_m")) s.shadow_center_m=vector(o["shadow_center_m"]);
    }
    if(sequence) {
        for(const auto& sample:p["samples"]) r.samples.push_back({camera(sample["camera"]),sample["time_seconds"].get<double>(),sample.value("camera_cut",false)});
        r.camera=r.samples.front().camera; r.environment.time_seconds=r.samples.front().time_seconds;
    } else if(p.contains("camera")) r.camera=camera(p["camera"]);
    return r;
}
}
Json render_job_result_schema() {
    auto properties=info_properties();
    properties["kind"]={{"type","string"},{"enum",{"capture","render_sequence"}}}; properties["draw_count"]=integer();
    properties["root_id"]=uuid(); properties["key"]=schema::string(32,32); properties["cache_hit"]=schema::boolean();
    return schema::object(std::move(properties));
}
Result<void> register_render_commands(CommandRegistry& registry,CaptureService& service,const SceneService& scene) {
    for(const bool sequence:{false,true}) {
        auto properties=info_properties(); properties["job_id"]=uuid();
        auto result=schema::object(std::move(properties),{"job_id","document_id","scene_id","revision","frame","width","height","output","provenance"});
        Json params{{"guard",schema::object({{"document_id",uuid()},{"revision",integer()}},{"document_id","revision"})},
            {"output",schema::string(1,1024)},{"width",integer(2048,1)},{"height",integer(2048,1)},
            {"exposure",number(0,1000000)},{"render_options",render_options_schema()},{"warmup_frames",integer(64)},
            {"profile",{{"type","string"},{"enum",{"strict","unlit_preview"}}}},
            {"validation",{{"type","string"},{"enum",{"if_available","required"}}}}};
        Json required={"guard","output"};
        if(sequence) {
            params["samples"]=schema::array(schema::object({{"camera",camera_schema()},{"time_seconds",number(-1e9,1e9)},{"camera_cut",schema::boolean()}},{"camera","time_seconds"}),1,128);
            required.push_back("samples");
        } else {
            params["camera"]=camera_schema(); params["time_seconds"]=number(-1e9,1e9);
            params["renderer"]={{"type","string"},{"enum",{"raster","pathtrace"}}};
            params["format"]={{"type","string"},{"enum",{"ppm","linear_json"}}}; params["pathtrace"]=path_schema();
        }
        auto added=registry.add({sequence ? "render.sequence" : "render.capture",
            sequence ? "Render frozen camera/time samples from empty history to an atomic JSON report" : "Capture the guarded scene with raster or fixed-spp path tracing to an atomic PPM/linear JSON",
            schema::object(std::move(params),std::move(required)),std::move(result),CommandEffect::external,false},
            [&service,&scene,sequence](const Json& p)->Result<Json> {
                if(p.value("renderer",std::string{"raster"})=="pathtrace") {
                    for(const char* name:{"exposure","time_seconds","render_options","warmup_frames"}) if(p.contains(name))
                        return std::unexpected(Error{ErrorCode::invalid_argument,std::string{"Raster-only parameter: "}+name});
                }
                auto guard=parse_edit_guard(p["guard"]); if(!guard) return std::unexpected(guard.error());
                auto ticket=service.capture(scene,*guard,request(p,sequence)); if(!ticket) return std::unexpected(ticket.error());
                const auto& i=ticket->info;
                return Json{{"job_id",ticket->job.to_string()},{"document_id",i.document.to_string()},{"scene_id",i.frame.scene.to_string()},
                    {"revision",i.frame.revision},{"frame",i.frame.frame},{"width",i.frame.width},{"height",i.frame.height},{"output",i.output},{"provenance",Json::parse(i.provenance)}};
            });
        if(!added) return added;
    }
    return {};
}
}
