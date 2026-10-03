#include <dk/operations/RenderOperations.hpp>
#include <dk/operations/SceneOperations.hpp>
namespace dk {
namespace {
Json integer(std::uint64_t maximum=UINT64_MAX,std::uint64_t minimum=0) {
    auto s=schema::integer(); s["minimum"]=minimum; s["maximum"]=maximum; return s;
}
Json uuid() { return schema::string(36,36); }
Json info_properties() { return {{"document_id",uuid()},{"scene_id",uuid()},{"revision",integer()},
    {"frame",integer(UINT64_MAX,1)},{"width",integer(2048,1)},{"height",integer(2048,1)},{"output",schema::string(1,1024)}}; }
Vec3d vector(const Json& value) { return {value[0].get<double>(),value[1].get<double>(),value[2].get<double>()}; }
}
Json render_job_result_schema() {
    auto properties=info_properties();
    properties["kind"]={{"type","string"},{"enum",{"capture"}}}; properties["draw_count"]=integer();
    properties["root_id"]=uuid(); properties["key"]=schema::string(32,32); properties["cache_hit"]=schema::boolean();
    return schema::object(std::move(properties));
}
Result<void> register_render_commands(CommandRegistry& registry,CaptureService& service,const SceneService& scene) {
    auto properties=info_properties(); properties["job_id"]=uuid();
    auto result=schema::object(std::move(properties),{"job_id","document_id","scene_id","revision","frame","width","height","output"});
    auto vec=schema::array(schema::number(),3,3);
    auto camera=schema::object({{"eye",vec},{"target",vec},{"up",vec},{"fov_y",schema::number()},
        {"near",schema::number()},{"far",schema::number()}},{"eye","target"});
    return registry.add({"render.capture","Capture the guarded in-memory scene asynchronously to a project-relative PPM",
        schema::object({{"guard",schema::object({{"document_id",uuid()},{"revision",integer()}},{"document_id","revision"})},
            {"output",schema::string(1,1024)},{"width",integer(2048,1)},{"height",integer(2048,1)},
            {"camera",std::move(camera)}, {"profile",{{"type","string"},{"enum",{"strict","unlit_preview"}}}},
            {"validation",{{"type","string"},{"enum",{"if_available","required"}}}}},{"guard","output"}),
        std::move(result),CommandEffect::external,false},
        [&service,&scene](const Json& p)->Result<Json> {
            auto guard=parse_edit_guard(p["guard"]); if (!guard) return std::unexpected(guard.error());
            CaptureRequest request; request.output=p["output"].get<std::string>();
            request.width=p.value("width",960U); request.height=p.value("height",540U);
            request.profile=p.value("profile",std::string{"unlit_preview"})=="strict" ? GltfImportProfile::strict : GltfImportProfile::unlit_preview;
            request.require_validation=p.value("validation",std::string{"if_available"})=="required";
            if (p.contains("camera")) {
                const auto& c=p["camera"]; request.camera.eye=vector(c["eye"]); request.camera.target=vector(c["target"]);
                if (c.contains("up")) request.camera.up=vector(c["up"]);
                request.camera.fov_y=c.value("fov_y",65.0); request.camera.near_plane=c.value("near",0.05); request.camera.far_plane=c.value("far",100.0);
            }
            auto ticket=service.capture(scene,*guard,request); if (!ticket) return std::unexpected(ticket.error());
            const auto& i=ticket->info;
            return Json{{"job_id",ticket->job.to_string()},{"document_id",i.document.to_string()},{"scene_id",i.frame.scene.to_string()},
                {"revision",i.frame.revision},{"frame",i.frame.frame},{"width",i.frame.width},{"height",i.frame.height},{"output",i.output}};
        });
}
}
