#include <dk/operations/EditorViewportOperations.hpp>
#include "EditorHostCommands.hpp"

namespace dk {
namespace {
Json number(double lo,double hi) { auto s=schema::number(); s["minimum"]=lo; s["maximum"]=hi; return s; }
Json integer(std::uint64_t lo,std::uint64_t hi) { auto s=schema::integer(); s["minimum"]=lo; s["maximum"]=hi; return s; }
Json choice(std::initializer_list<const char*> values) { auto s=schema::string(); s["enum"]=values; return s; }
Json guard_schema() { return schema::object({{"view_id",schema::string(36,36)},{"document_id",schema::string(36,36)},{"revision",integer(0,UINT64_MAX)}},{"view_id","document_id","revision"}); }
Json guard_json(EditorViewportGuard g) { return {{"view_id",g.view_id.to_string()},{"document_id",g.document_id.to_string()},{"revision",g.revision}}; }
Result<EditorViewportGuard> read_guard(const Json& value) {
    auto view=EditorViewId::parse(value.at("view_id").get<std::string>()); if(!view) return std::unexpected(view.error());
    auto doc=DocumentId::parse(value.at("document_id").get<std::string>()); if(!doc) return std::unexpected(doc.error());
    return EditorViewportGuard{*view,*doc,value.at("revision").get<std::uint64_t>()};
}
Json properties() {
    return {{"renderer",choice({"raster","pathtrace"})},{"medium_mode",choice({"surface_only","scene_media"})},
        {"direct_lighting",choice({"classic","restir_di"})},{"initial_candidates",integer(1,64)},
        {"temporal_reuse",schema::boolean()},{"spatial_reuse",schema::boolean()},
        {"target_spp",integer(1,UINT32_MAX)},{"seed",integer(0,UINT32_MAX)},{"bounces",integer(1,32)},
        {"environment_rgb",schema::array(number(0,1000),3,3)},{"sky_enabled",schema::boolean()},{"sun",schema::boolean()},
        {"exposure_ev",number(-16,16)},{"raster_exposure",number(0,1e6)},{"unlit_inspection",schema::boolean()},
        {"voxel_shading",choice({"unlit","directional"})},{"voxel_light_direction",schema::array(number(-1,1),3,3)},
        {"voxel_irradiance",schema::array(number(0,1e6),3,3)},
        {"time_seconds",number(-1e9,1e9)},{"cloud_albedo",number(0,1)},{"cloud_anisotropy",number(-.95,.95)}};
}
Json object_with_all(Json fields) { auto keys=Json::array(); for(const auto& [name,value] : fields.items()) { (void)value; keys.push_back(name); } return schema::object(std::move(fields),std::move(keys)); }
Json settings_json(const EditorViewportSettings& s) {
    const auto& p=s.path;
    return {{"renderer",s.path_trace ? "pathtrace" : "raster"},{"medium_mode",p.scene_media ? "scene_media" : "surface_only"},
        {"direct_lighting",p.restir_di ? "restir_di" : "classic"},{"initial_candidates",p.restir_candidates},
        {"temporal_reuse",p.restir_temporal},{"spatial_reuse",p.restir_spatial},{"target_spp",p.target},{"seed",p.seed},{"bounces",p.bounces},
        {"environment_rgb",{p.sky.x(),p.sky.y(),p.sky.z()}},{"sky_enabled",p.sky_enabled},{"sun",p.sun},{"exposure_ev",p.exposure},
        {"raster_exposure",s.raster_exposure},{"unlit_inspection",s.unlit_inspection},
        {"voxel_shading",s.voxel.directional ? "directional" : "unlit"},
        {"voxel_light_direction",{s.voxel.light_direction.x(),s.voxel.light_direction.y(),s.voxel.light_direction.z()}},
        {"voxel_irradiance",{s.voxel.irradiance.x(),s.voxel.irradiance.y(),s.voxel.irradiance.z()}},
        {"time_seconds",p.time_seconds},{"cloud_albedo",p.cloud_albedo},{"cloud_anisotropy",p.cloud_anisotropy},
        {"paused",p.paused},{"reset_sequence",p.reset}};
}
Json result_schema() {
    auto settings=properties(); settings["paused"]=schema::boolean(); settings["reset_sequence"]=integer(0,UINT64_MAX);
    auto presentation=object_with_all({{"observed_guard",guard_schema()},{"shown_guard",schema::nullable(guard_schema())},
        {"document_id",schema::string(36,36)},{"scene_revision",integer(0,UINT64_MAX)},{"camera_revision",integer(0,UINT64_MAX)},
        {"generation",integer(0,UINT64_MAX)},{"width",integer(0,UINT32_MAX)},{"height",integer(0,UINT32_MAX)},
        {"spp",integer(0,UINT32_MAX)},{"target_spp",integer(0,UINT32_MAX)},{"has_image",schema::boolean()},{"current",schema::boolean()},
        {"renderer",choice({"raster","pathtrace"})},{"phase",schema::string(0,256)},{"error",schema::string(0,8192)}});
    return object_with_all({{"guard",guard_schema()},{"settings",object_with_all(std::move(settings))},
        {"capabilities",object_with_all({{"pathtracing",schema::boolean()},{"unavailable",schema::string(0,8192)}})},
        {"blocked_reason",schema::string(0,8192)},{"presentation",std::move(presentation)}});
}
template<class T> void assign(const Json& p,const char* name,T& value) { if(p.contains(name)) value=p[name].get<T>(); }
EditorViewportSettings patch(const Json& p,EditorViewportSettings s) {
    auto& v=s.path;
    if(p.contains("renderer")) s.path_trace=p["renderer"]=="pathtrace";
    if(p.contains("medium_mode")) v.scene_media=p["medium_mode"]=="scene_media";
    if(p.contains("direct_lighting")) v.restir_di=p["direct_lighting"]=="restir_di";
    assign(p,"initial_candidates",v.restir_candidates); assign(p,"temporal_reuse",v.restir_temporal); assign(p,"spatial_reuse",v.restir_spatial);
    assign(p,"target_spp",v.target); assign(p,"seed",v.seed); assign(p,"bounces",v.bounces);
    assign(p,"sky_enabled",v.sky_enabled); assign(p,"sun",v.sun); assign(p,"exposure_ev",v.exposure);
    if(p.contains("environment_rgb")) for(int i=0;i<3;++i) v.sky[i]=p["environment_rgb"][i].get<double>();
    assign(p,"raster_exposure",s.raster_exposure); assign(p,"unlit_inspection",s.unlit_inspection);
    if(p.contains("voxel_shading")) s.voxel.directional=p["voxel_shading"]=="directional";
    if(p.contains("voxel_light_direction")) for(int i=0;i<3;++i) s.voxel.light_direction[i]=p["voxel_light_direction"][i].get<double>();
    if(p.contains("voxel_irradiance")) for(int i=0;i<3;++i) s.voxel.irradiance[i]=p["voxel_irradiance"][i].get<double>();
    assign(p,"time_seconds",v.time_seconds); assign(p,"cloud_albedo",v.cloud_albedo); assign(p,"cloud_anisotropy",v.cloud_anisotropy);
    return s;
}
}
Json editor_viewport_state_json(const EditorViewportState& s) {
    const auto& p=s.presentation;
    return {{"guard",guard_json(s.guard)},{"settings",settings_json(s.settings)},
        {"capabilities",{{"pathtracing",s.capabilities.pathtracing},{"unavailable",s.capabilities.unavailable}}},{"blocked_reason",s.blocked_reason},
        {"presentation",{{"observed_guard",guard_json(p.observed)},{"shown_guard",p.shown ? guard_json(*p.shown) : Json(nullptr)},
            {"document_id",p.document_id.to_string()},{"scene_revision",p.scene_revision},{"camera_revision",p.camera_revision},
            {"generation",p.generation},{"width",p.width},{"height",p.height},{"spp",p.spp},{"target_spp",p.target_spp},
            {"has_image",p.has_image},{"current",p.current},{"renderer",p.renderer},{"phase",p.phase},{"error",p.error}}}};
}
Result<void> register_editor_viewport_commands(CommandRegistry& registry,std::function<std::shared_ptr<EditorViewportService>()> host) {
    std::vector<EditorCommandEntry> entries;
    for(const std::string action : {"query","set","pause","resume","reset"}) {
        auto fields=action=="set" ? properties() : Json::object();
        if(action!="query") fields["guard"]=guard_schema();
        auto help=Json::object();
        if(action=="set") {
            auto defaults=settings_json({});
            const auto setting_fields=properties();
            for(const auto& [name,value] : setting_fields.items()) { (void)value;
                help["/properties/"+name]={{"examples",Json::array({defaults[name]})},{"description","Edit viewport session setting; omitted fields preserve current values. Does not edit the scene or undo history."}};
            }
            help["/properties/target_spp"]["description"]="Absolute target spp, not an additional iteration count; preserves completed samples.";
            help["/properties/exposure_ev"]["unit"]="EV";
            help["/properties/exposure_ev"]["description"]="Path display exposure; preserves accumulated samples.";
            help["/properties/time_seconds"]["unit"]="s";
            help["/properties/sky_enabled"]["description"]="Enable the constant environment RGB radiance; retains the RGB value and does not edit scene atmosphere.";
            help["/properties/raster_exposure"]["description"]="Raster environment exposure multiplier; independent of path display EV.";
            help["/properties/voxel_light_direction"]["description"]="Unit world direction towards the light; length must equal one (tolerance 1e-6).";
            help["/properties/voxel_shading"]["description"]="Solid voxel raster shading; directional requires nonmetallic palettes and no scene atmosphere/clouds. Use explicit unlit_inspection to omit scene environment.";
            help["/properties/guard"]={{"description","Use the latest view_id, document_id and viewport revision from editor.viewport.query; never a scene guard."}};
        } else help[""]={{"description",action=="query" ? "Inspect the Edit viewport guard, settings, capabilities and completed image source." :
            "Guarded Edit viewport action. Accepted state is distinct from GPU completion."}};
        auto name="editor.viewport."+action;
        entries.push_back({{name,"Edit viewport: "+action,
            schema::object(std::move(fields),action=="query" ? Json::array() : Json::array({"guard"})),result_schema(),
            action=="query" ? CommandEffect::query : CommandEffect::control,false,Json{{"version",1},{"fields",std::move(help)}}},
            [host,action](const Json& params)->Result<Json> {
                auto service=host(); if(!service) return std::unexpected(Error{ErrorCode::invalid_state,"Editor viewport host is detached"});
                if(action=="query") { auto state=service->query(); if(!state) return std::unexpected(state.error()); return editor_viewport_state_json(*state); }
                auto guard=read_guard(params.at("guard")); if(!guard) return std::unexpected(guard.error());
                Result<EditorViewportState> result=std::unexpected(Error{ErrorCode::invalid_argument,"At least one viewport setting is required"});
                if(action=="set" && params.size()>1) {
                    bool explicit_path=false;
                    for(const auto& [key,value] : params.items()) { (void)value; explicit_path|=key!="guard" && key!="renderer" && key!="raster_exposure" && key!="unlit_inspection" && !key.starts_with("voxel_"); }
                    result=service->set(*guard,patch(params,service->settings()),explicit_path);
                } else if(action!="set") result=service->control(*guard,action=="pause" ? EditorViewportControl::pause : action=="resume" ? EditorViewportControl::resume : EditorViewportControl::reset);
                if(!result) return std::unexpected(result.error()); return editor_viewport_state_json(*result);
            }});
    }
    // Validate the complete extension before publishing any names to a live Runtime.
    auto camera=editor_camera_commands(host,result_schema());
    for(auto& entry : camera) entries.push_back(std::move(entry));
    CommandRegistry preflight;
    for(const auto& provider : registry.catalog_snapshot().completion_providers) {
        auto declared=preflight.register_completion_provider(provider); if(!declared) return declared;
    }
    for(const auto& e : entries) {
        if(registry.describe(e.descriptor.name)) return std::unexpected(Error{ErrorCode::conflict,"Viewport command already registered"});
        auto checked=preflight.add(e.descriptor,e.handler); if(!checked) return checked;
    }
    for(auto& e : entries) { auto added=registry.add(std::move(e.descriptor),std::move(e.handler)); if(!added) return added; }
    return {};
}
}
