#include <dk/services/CaptureService.hpp>
#include <dk/render/DiskScene.hpp>
#include <dk/memory/MemorySystem.hpp>
#include <dk/io/File.hpp>
#include <dk/profiling/Profiler.hpp>
#include <dk/math/Projection.hpp>
#include <nlohmann/json.hpp>
#include <cmath>
#include <numbers>
#include <chrono>
#include <algorithm>
#include "CaptureInternal.hpp"

namespace dk {
namespace {
using capture_detail::Completion;
template<class T> T take(Result<T> r) { if (!r) throw std::move(r.error()); return std::move(*r); }
void check(Result<void> r) { if (!r) throw std::move(r.error()); }
void require(bool valid,const char* text,ErrorCode code=ErrorCode::invalid_argument) {
    if (!valid) throw Error{code,text};
}
void checkpoint(std::stop_token stop) { require(!stop.stop_requested(),"Capture cancelled",ErrorCode::invalid_state); }
memory::MemorySystem system() { auto r=memory::MemorySystem::create(); if (!r) throw std::bad_alloc{}; return std::move(*r); }
memory::ResourceHandle heap(memory::MemorySystem& m,const char* name,memory::DomainCategory category) {
    auto r=m.create_heap({name,category}); if (!r) throw std::bad_alloc{}; return *r;
}
render::ViewDescription view_description(const CaptureRequest& r) {
    require(r.width && r.height && r.width<=2048 && r.height<=2048,"Capture extent must be 1..2048");
    const auto& c=r.camera;
    require(c.orthographic ? (c.ortho_height && !c.explicit_fov_y) : !c.ortho_height,
        "Orthographic requires ortho_height and forbids fov_y; perspective forbids ortho_height");
    require(c.eye.allFinite() && c.target.allFinite() && c.up.allFinite() &&
        std::isfinite(c.fov_y) && c.fov_y>1 && c.fov_y<179 && std::isfinite(c.near_plane) &&
        std::isfinite(c.far_plane) && c.near_plane>0 && c.far_plane>c.near_plane,"Invalid capture camera");
    const Vec3d delta=c.target-c.eye;
    require(delta.allFinite() && delta.norm()>1e-9 && std::isfinite(delta.norm()) &&
        c.up.norm()>1e-9 && std::isfinite(c.up.norm()),"Degenerate capture camera direction");
    const Vec3d forward=delta.normalized(), cross=forward.cross(c.up.normalized());
    require(cross.norm()>1e-9,"Capture camera up is parallel to its direction");
    const Vec3d right=cross.normalized(), up=right.cross(forward);
    Mat4d world=Mat4d::Identity(); world.block<3,1>(0,0)=right; world.block<3,1>(0,1)=up;
    world.block<3,1>(0,2)=-forward; world.block<3,1>(0,3)=c.eye;
    render::ViewDescription v{r.width,r.height,0}; v.camera_world=take(Transformd::from_matrix(world));
    const double aspect=static_cast<double>(r.width)/r.height;
    v.projection=c.orthographic ? take(orthographic_projection({*c.ortho_height,c.near_plane,c.far_plane},aspect)) :
        take(perspective_projection({radians(c.fov_y),c.near_plane,c.far_plane},aspect));
    return v;
}
nlohmann::json environment_json(const EnvironmentDescription& e) {
    return {{"version", 2}, {"enabled", e.enabled},
        {"clouds", {{"enabled", e.clouds.enabled},
            {"seed", e.clouds.seed},
            {"generator_version", e.clouds.generator_version},
            {"weather_resolution", e.clouds.weather_resolution},
            {"base_resolution", e.clouds.base_resolution},
            {"detail_resolution", e.clouds.detail_resolution},
            {"bottom_height_m", e.clouds.bottom_height_m},
            {"top_height_m", e.clouds.top_height_m},
            {"coverage", e.clouds.coverage},
            {"density", e.clouds.density},
            {"extinction_per_m", e.clouds.extinction_per_m},
            {"weather_scale_m", e.clouds.weather_scale_m},
            {"shape_scale_m", e.clouds.shape_scale_m},
            {"detail_scale_m", e.clouds.detail_scale_m},
            {"bottom_fade_fraction", e.clouds.bottom_fade_fraction},
            {"top_fade_fraction", e.clouds.top_fade_fraction},
            {"wind_velocity_mps", e.clouds.wind_velocity_mps}}},
        {"atmosphere", {{"planet_center_m", e.atmosphere.planet_center_m},
        {"ground_radius_m", e.atmosphere.ground_radius_m},
        {"atmosphere_height_m", e.atmosphere.atmosphere_height_m},
        {"rayleigh_scattering_per_m", e.atmosphere.rayleigh_scattering_per_m},
        {"rayleigh_scale_height_m", e.atmosphere.rayleigh_scale_height_m},
        {"mie_scattering_per_m", e.atmosphere.mie_scattering_per_m},
        {"mie_extinction_per_m", e.atmosphere.mie_extinction_per_m},
        {"mie_scale_height_m", e.atmosphere.mie_scale_height_m},
        {"mie_anisotropy", e.atmosphere.mie_anisotropy},
        {"absorption_extinction_per_m", e.atmosphere.absorption_extinction_per_m},
        {"absorption_center_height_m", e.atmosphere.absorption_center_height_m},
        {"absorption_half_width_m", e.atmosphere.absorption_half_width_m},
        {"ground_albedo", e.atmosphere.ground_albedo}}},
        {"sun", {{"direction", e.sun.direction},
        {"irradiance", e.sun.irradiance},
        {"angular_radius_rad", e.sun.angular_radius_rad}}}};
}
nlohmann::json camera_json(const CaptureCamera& c) {
    auto vector=[](const Vec3d& v) { return nlohmann::json::array({v.x(),v.y(),v.z()}); };
    nlohmann::json value={{"eye",vector(c.eye)},{"target",vector(c.target)},{"up",vector(c.up)},
        {"projection",c.orthographic ? "orthographic" : "perspective"},{"near",c.near_plane},{"far",c.far_plane}};
    if(c.orthographic) { value["ortho_height"]=*c.ortho_height; value["primary_ray_origin"]="near_plane"; }
    else value["fov_y"]=c.fov_y;
    return value;
}
nlohmann::json settings_json(const render::EnvironmentRenderSettings& s) {
    return {{"mode",s.mode==render::CloudRenderMode::reference ? "reference" : "temporal"},
        {"cloud_shadows",s.cloud_shadows},{"shadow_center_m",{s.shadow_center_m.x(),s.shadow_center_m.y(),s.shadow_center_m.z()}},
        {"shadow_extent_m",s.shadow_extent_m},{"shadow_resolution",s.shadow_resolution}};
}
std::string provenance(const CaptureRequest& r,const EnvironmentDescription& environment) {
    const auto& clouds=environment.clouds;
    const bool cloudy=clouds.enabled && clouds.coverage>0 && clouds.density>0 && clouds.extinction_per_m>0;
    auto value=nlohmann::json{{"environment",environment_json(environment)},{"camera",camera_json(r.camera)},
        {"exposure",r.exposure},{"time_seconds",r.environment.time_seconds},
        {"quality",cloudy ? (r.environment.mode==render::CloudRenderMode::reference ? "cloud_reference_v1" : "cloud_temporal_v1") : environment.enabled ? "atmosphere_v1" : "unlit_v1"},
        {"render_options",settings_json(r.environment)},{"warmup_frames",r.warmup_frames},{"sample_count",r.samples.empty() ? 1 : r.samples.size()},
        {"profile",r.profile==GltfImportProfile::strict ? "strict" : "unlit_preview"},{"shader_fingerprint",nullptr}};
    value["renderer"]=r.pathtrace ? "pathtrace" : "raster";
    value["format"]=!r.samples.empty() ? "sequence_json" : r.linear_json ? "linear_json" : "ppm";
    value["pathtrace"]=nullptr;
    if(r.pathtrace) {
        const auto& p=*r.path;
        value["quality"]=p.version==2 ? "pathtrace_v2" : "pathtrace_v1";
        value["pathtrace"]={{"version",p.version},{"target_spp",p.target_spp},{"seed",p.seed},{"max_bounces",p.max_bounces},
            {"rr_start",p.rr_start},{"environment_rgb",{p.environment_rgb.x(),p.environment_rgb.y(),p.environment_rgb.z()}},
            {"sun",p.sun},{"surface_only",p.surface_only},{"exposure_ev",p.exposure_ev}};
        if(p.version==2) {
            auto& path=value["pathtrace"]; path.erase("surface_only"); path["medium_mode"]=p.scene_media ? "scene_media" : "surface_only";
            value["geometry_fingerprint"]=nullptr;
            if(p.scene_media) {
                path["time_seconds"]=p.time_seconds; path["cloud_albedo"]=p.cloud_albedo; path["cloud_anisotropy"]=p.cloud_anisotropy;
                path["max_tracking_steps"]=p.max_tracking_steps; path["max_steps_per_path"]=p.max_steps_per_path;
                value["time_seconds"]=p.time_seconds; value["medium_model_version"]=1; value["cloud_field_hash"]=nullptr;
            }
        }
        value["model_version"]=p.scene_media ? 4 : 3; value["sampler_version"]=p.scene_media ? 2 : 1; value["material_model"]="metallic_roughness";
        value["estimator"]="mis"; value["actual_spp"]=0; value["input_fingerprint"]=nullptr;
        value["input_fingerprint_version"]=3;
    }
    return value.dump();
}
Result<JobQueue::Payload> render_capture(memory::ResourceHandle resource,const SceneReadSnapshot& input,
    const CaptureRequest& request,const CaptureInfo& info,render::ViewDescription view,
    const std::filesystem::path& shader_directory,const std::filesystem::path& output,std::stop_token stop) {
    DK_PROFILE_ZONE("Capture.Render");
    try {
        checkpoint(stop);
        render::DiskSceneOptions options; options.profile=request.profile; options.stop=stop;
        auto source=take(render::DiskScene::load_snapshot(resource,input.project,input.scene,options));
        checkpoint(stop);
        auto completion=std::make_shared<Completion>(); completion->output=output;
        graphics::DeviceOptions device_options;
        device_options.validation=request.require_validation ? graphics::ValidationMode::required : graphics::ValidationMode::if_available;
        auto device=take(graphics::Device::create(resource,device_options));
        auto queue=take(graphics::SubmissionQueue::create(resource,std::move(device)));
        render::FrameInfo frame_info;
        {
            auto assets=take(source.upload(queue)); checkpoint(stop);
            const auto fingerprint=take(render::shader_fingerprint(shader_directory,input.scene.environment().enabled || input.scene.environment().clouds.enabled));
            auto pipeline=take(render::ScenePipeline::create(resource,queue,shader_directory)); checkpoint(stop);
            auto settings=render::unlit_preview_settings(); settings.exposure=request.exposure; settings.linear_readback=!request.samples.empty();
            completion->provenance=nlohmann::json::parse(info.provenance);
            completion->provenance["shader_fingerprint"]=fingerprint;
            nlohmann::json frames=nlohmann::json::array();
            const auto count=request.samples.empty() ? 1 : request.samples.size();
            auto environment=request.environment; environment.view_id=1; environment.camera_revision=0;
            const auto started=std::chrono::steady_clock::now();
            for(std::size_t i=0;i<count;++i) {
                if(!request.samples.empty()) {
                    CaptureRequest sample_request; sample_request.width=request.width; sample_request.height=request.height; sample_request.camera=request.samples[i].camera;
                    view=view_description(sample_request); view.frame=info.frame.frame;
                    environment.time_seconds=request.samples[i].time_seconds;
                    if(request.samples[i].camera_cut) ++environment.camera_revision;
                }
                auto render_view=take(render::RenderView::create(source.scene(),view));
                for(unsigned warm=0;warm<=(i==0 ? request.warmup_frames : 0);++warm) {
                    checkpoint(stop);
                    const auto frame_start=std::chrono::steady_clock::now();
                    auto frame=take(pipeline.render_environment(queue,render_view,assets,settings,environment));
                    require(take(frame.wait(queue)),"Capture GPU wait did not complete",ErrorCode::invalid_state);
                    checkpoint(stop);
                    if(i==0 && warm<request.warmup_frames) continue;
                    frame_info=frame.info();
                    ByteBuffer rgba(static_cast<std::size_t>(request.width)*request.height*4); check(frame.read_rgba8(rgba));
                    if(request.samples.empty()) {
                        const std::string header="P6\n"+std::to_string(request.width)+" "+std::to_string(request.height)+"\n255\n";
                        const auto bytes=std::as_bytes(std::span{header.data(),header.size()});
                        completion->ppm.reserve(bytes.size()+rgba.size()/4*3);
                        completion->ppm.insert(completion->ppm.end(),bytes.begin(),bytes.end());
                        for(std::size_t j=0;j<rgba.size();j+=4) completion->ppm.insert(completion->ppm.end(),rgba.begin()+j,rgba.begin()+j+3);
                    } else {
                        std::vector<std::array<float,4>> linear(std::size_t{request.width}*request.height); check(frame.read_linear_rgba32f(linear));
                        std::array<double,3> low{INFINITY,INFINITY,INFINITY},high{-INFINITY,-INFINITY,-INFINITY},mean{};
                        for(const auto& pixel:linear) for(unsigned c=0;c<4;++c) {
                            require(std::isfinite(pixel[c]),"Nonfinite sequence HDR output",ErrorCode::internal_error);
                            if(c<3) { low[c]=std::min(low[c],double(pixel[c])); high[c]=std::max(high[c],double(pixel[c])); mean[c]+=pixel[c]/double(linear.size()); }
                        }
                        std::vector<unsigned char> encoded; encoded.reserve(rgba.size()); for(auto b:rgba) encoded.push_back(std::to_integer<unsigned char>(b));
                        const auto history=frame.cloud_optimization();
                        const double wall=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-frame_start).count();
                        frames.push_back({{"index",i},{"camera",camera_json(request.samples[i].camera)},{"time_seconds",environment.time_seconds},
                            {"camera_cut",request.samples[i].camera_cut},{"rgba8",std::move(encoded)},{"linear_rgba",std::move(linear)},
                            {"linear_summary",{{"min",low},{"max",high},{"mean",mean}}},{"wall_ms",wall},{"gpu_ms",nullptr},
                            {"history",{{"available",history.history_available},{"source_sequence",history.source_sequence},{"candidate_sequence",history.candidate_sequence},
                                {"reset",render::cloud_history_reset_name(history.reset)}}},{"shadow_generation",frame.cloud_shadow_generation()}});
                    }
                }
            }
            require(take(render::shader_fingerprint(shader_directory,input.scene.environment().enabled || input.scene.environment().clouds.enabled))==fingerprint,"Render shaders changed during capture",ErrorCode::conflict);
            if(!request.samples.empty()) {
                const auto& adapter=queue.device().adapter();
                const auto report=nlohmann::json{{"schema_version",1},{"kind","decker.render_sequence"},{"status","succeeded"},
                    {"document_id",info.document.to_string()},{"scene_id",frame_info.scene.to_string()},{"revision",frame_info.revision},{"frame",info.frame.frame},
                    {"width",request.width},{"height",request.height},{"provenance",completion->provenance},
                    {"device",{{"name",adapter.properties.deviceName.data()},{"driver",adapter.driver.driverInfo.data()}}},
                    {"wall_ms",std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-started).count()},
                    {"gpu_ms",nullptr},{"frames",std::move(frames)}}.dump();
                require(report.size()<=64U*1024U*1024U,"Sequence JSON exceeds 64 MiB");
                const auto bytes=std::as_bytes(std::span{report.data(),report.size()}); completion->ppm.assign(bytes.begin(),bytes.end());
            }
        }
        check(queue.close());
        require(queue.device().validation_errors()==0 && queue.device().validation_warnings()==0,"Capture Vulkan validation failed",ErrorCode::internal_error);
        checkpoint(stop);
        completion->summary=nlohmann::json{{"kind",request.samples.empty() ? "capture" : "render_sequence"},{"document_id",info.document.to_string()},
            {"scene_id",frame_info.scene.to_string()},{"revision",frame_info.revision},{"frame",frame_info.frame},
            {"width",frame_info.width},{"height",frame_info.height},{"output",info.output},
            {"draw_count",frame_info.draw_count},{"provenance",std::move(completion->provenance)}}.dump();
        require(completion->summary.size()<=4096,"Capture summary exceeds Job metadata limit");
        return completion;
    } catch (Error& e) { return std::unexpected(e.with_context(request.samples.empty() ? "render.capture" : "render.sequence")); }
}
}
struct CaptureService::Impl {
    memory::MemorySystem memory=system();
    memory::ResourceHandle render_heap=heap(memory,"runtime-capture",memory::DomainCategory::render);
    memory::ResourceHandle jobs_heap=heap(memory,"capture-jobs",memory::DomainCategory::jobs);
    memory::ThreadContext context{memory,render_heap};
    JobQueue queue;
    std::filesystem::path shaders;
    std::uint64_t next_frame=1;
    Impl(std::filesystem::path path,std::function<void()> wake)
        : queue(jobs_heap,{2,3,64,192U*1024U*1024U},std::move(wake)),shaders(std::move(path)) {}
};
CaptureService::CaptureService(std::unique_ptr<Impl> impl):impl_(std::move(impl)) {}
Result<std::unique_ptr<CaptureService>> CaptureService::create(std::filesystem::path shaders,std::function<void()> wake) {
    if (shaders.empty()) return std::unexpected(Error{ErrorCode::invalid_argument,"Capture requires a shader directory"});
    return std::unique_ptr<CaptureService>(new CaptureService(std::make_unique<Impl>(std::move(shaders),std::move(wake))));
}
CaptureService::~CaptureService() { close(); }
Result<CaptureTicket> CaptureService::capture(const SceneService& scene,EditGuard guard,const CaptureRequest& request) {
    DK_PROFILE_ZONE("Capture.Submit");
    auto& s=*impl_; memory::ExecutionScope scope(s.context,s.render_heap);
    try {
        auto view=view_description(request);
        require(request.pathtrace==request.path.has_value(),"Path tracing requires renderer=pathtrace and explicit pathtrace settings");
        require(!request.linear_json || request.pathtrace,"linear_json requires renderer=pathtrace");
        if(request.pathtrace) {
#ifndef DK_CAPTURE_PATHTRACING
            throw Error{ErrorCode::not_supported,"Path tracing capture was not built"};
#endif
            const auto& p=*request.path;
            require(request.samples.empty() && request.profile==GltfImportProfile::unlit_preview && request.exposure==1 &&
                request.warmup_frames==0 && request.environment==render::EnvironmentRenderSettings{},"Path capture requires unlit_preview and no raster sequence/exposure/warmup/options");
            require((p.version==1 || p.version==2) && p.target_spp>=1 && p.target_spp<=65536 && p.max_bounces>=1 && p.max_bounces<=16 &&
                p.rr_start<=p.max_bounces && p.environment_rgb.allFinite() && p.environment_rgb.minCoeff()>=0 && p.environment_rgb.maxCoeff()<=1e6 &&
                std::isfinite(p.exposure_ev) && std::abs(p.exposure_ev)<=32,"Invalid pathtrace settings");
            require((p.version==2 || !p.scene_media) && std::isfinite(p.time_seconds) && std::abs(p.time_seconds)<=1e9 &&
                std::isfinite(p.cloud_albedo) && p.cloud_albedo>=0 && p.cloud_albedo<=1 &&
                std::isfinite(p.cloud_anisotropy) && std::abs(p.cloud_anisotropy)<=.95 &&
                p.max_tracking_steps>=1 && p.max_tracking_steps<=(1ull<<28) && p.max_steps_per_path>=1 && p.max_steps_per_path<=(1u<<20),
                "Invalid pathtrace media settings");
            require(!request.linear_json || std::size_t{request.width}*request.height<=524288,"Path linear report allows at most 524288 pixels");
        }
        check(render::validate_environment_render_settings(request.environment));
        require(request.warmup_frames<=64 && (request.environment.mode==render::CloudRenderMode::temporal || !request.warmup_frames),"Warmup requires temporal mode and at most 64 frames");
        require(request.samples.size()<=128,"Sequence allows at most 128 samples");
        if(!request.samples.empty()) {
            const auto pixels=std::size_t{request.width}*request.height;
            require(pixels<=65536 && pixels*request.samples.size()<=262144,"Sequence pixel/readback budget exceeded");
            for(const auto& sample:request.samples) {
                auto sample_request=request; sample_request.camera=sample.camera; static_cast<void>(view_description(sample_request));
                auto render=request.environment; render.time_seconds=sample.time_seconds; check(render::validate_environment_render_settings(render));
            }
        }
        require(std::isfinite(request.exposure) && request.exposure>=0 && request.exposure<=1000000,"Capture exposure must be finite in [0,1000000]");
        require(request.profile==GltfImportProfile::strict || request.profile==GltfImportProfile::unlit_preview,"Invalid capture profile");
        require(!request.output.empty() && request.output.size()<=1024,"Capture output must be 1..1024 UTF-8 bytes");
        auto relative=take(path_from_utf8(request.output));
        require(relative.extension()==(request.samples.empty() && !request.linear_json ? ".ppm" : ".json"),"Output extension must match ppm or linear_json/sequence format");
        auto snapshot=take(scene.read_snapshot(guard));
        const auto& clouds=snapshot.scene.environment().clouds;
        if(!request.samples.empty() && request.environment.mode==render::CloudRenderMode::reference && clouds.enabled && clouds.coverage>0 && clouds.density>0 && clouds.extinction_per_m>0)
            require(std::size_t{request.width}*request.height<=4096,"Reference cloud sequence allows at most 4096 pixels");
        const auto output=take(snapshot.project.paths().resolve(relative));
        std::error_code error;
        require(std::filesystem::is_directory(output.parent_path(),error) && !error,"Capture output parent must exist",ErrorCode::io_error);
        const auto output_name=take(path_to_utf8(output.lexically_relative(snapshot.project.paths().root())));
        const auto snapshot_bytes=snapshot.scene.logical_bytes();
        require(snapshot_bytes<=32U*1024U*1024U,"Capture scene snapshot exceeds 32 MiB");
        std::size_t input_bytes=snapshot_bytes+sizeof(SceneReadSnapshot)+sizeof(CaptureRequest)+sizeof(Completion)+
            (output.native().size()+s.shaders.native().size()+snapshot.project.paths().root().native().size())*sizeof(std::filesystem::path::value_type)+
            snapshot.project.description().name.size()+snapshot.project.description().scene.size()+request.output.size()+8192;
        for (const auto& asset : snapshot.project.description().assets) input_bytes+=sizeof(AssetRecord)+asset.path.size()+64;
        input_bytes+=request.linear_json ? static_cast<std::size_t>(request.width)*request.height*256 : request.samples.empty() ? static_cast<std::size_t>(request.width)*request.height*7 :
            static_cast<std::size_t>(request.width)*request.height*request.samples.size()*256+request.samples.size()*sizeof(CaptureSample);
        require(s.next_frame!=UINT64_MAX,"Capture frame counter exhausted",ErrorCode::invalid_state);
        view.frame=s.next_frame;
        CaptureInfo info{snapshot.state.document_id,{snapshot.scene.id(),snapshot.scene.revision(),view.frame,request.width,request.height,0},output_name,provenance(request,snapshot.scene.environment())};
        auto work=[input=std::move(snapshot),request,info,view,resource=s.render_heap,shaders=s.shaders,output](std::stop_token stop,const JobQueue::Reporter& report)->Result<JobQueue::Payload> {
#ifdef DK_CAPTURE_PATHTRACING
            if(request.pathtrace) return capture_detail::render_path_capture(resource,input,request,info,view,output,stop,report);
#endif
            return render_capture(resource,input,request,info,view,shaders,output,stop);
        };
        auto job=request.pathtrace ? s.queue.submit(std::move(work),input_bytes,{"queued","spp",0,0,request.path->target_spp}) :
            s.queue.submit([work=std::move(work)](std::stop_token stop) { return work(stop,{}); },input_bytes);
        if (!job) return std::unexpected(job.error());
        ++s.next_frame;
        return CaptureTicket{*job,std::move(info)};
    } catch (Error& e) { return std::unexpected(std::move(e)); }
}
void CaptureService::pump() {
    auto& s=*impl_; memory::ExecutionScope scope(s.context,s.render_heap);
    s.queue.drain([](JobId,const JobQueue::Payload& payload)->Result<std::string> {
        const auto value=std::static_pointer_cast<const Completion>(payload);
        // All result storage is prepared before the single external commit point.
        auto summary=value->summary;
        auto saved=write_file_bytes_atomic(value->output,value->ppm);
        if (!saved) return std::unexpected(saved.error());
        return summary;
    });
}
Result<JobSnapshot> CaptureService::job(JobId id) const { return impl_->queue.query(id); }
Result<JobCancel> CaptureService::cancel(JobId id) { return impl_->queue.request_cancel(id); }
JobLimits CaptureService::limits() const noexcept { return impl_->queue.limits(); }
void CaptureService::rethrow_failure() const { impl_->queue.rethrow_failure(); }
void CaptureService::close() noexcept { impl_->queue.close(); }
}
