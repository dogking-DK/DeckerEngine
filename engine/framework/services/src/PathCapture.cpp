#include "CaptureInternal.hpp"
#include "CaptureRestir.hpp"
#include <dk/render/PathTraceRenderer.hpp>
#include <dk/render/DiskScene.hpp>
#include <dk/graphics/CommandEncoder.hpp>
#include <dk/graphics/Transfer.hpp>
#include <dk/io/File.hpp>
#include <chrono>
#include <thread>
#include <format>
#include <cmath>

namespace dk::capture_detail {
namespace {
template<class T> T take(Result<T> r) { if(!r) throw std::move(r.error()); return std::move(*r); }
void check(Result<void> r) { if(!r) throw std::move(r.error()); }
void require(bool v,const char* message) { if(!v) throw Error{ErrorCode::invalid_state,message}; }
void checkpoint(std::stop_token stop) { require(!stop.stop_requested(),"Path capture cancelled"); }
std::string fingerprint(const std::filesystem::path& directory,bool restir) {
    std::uint64_t hash=14695981039346656037ull;
    const auto append=[&](const std::filesystem::path& root,const std::string& name) {
        for(char c:name) { hash^=static_cast<unsigned char>(c); hash*=1099511628211ull; }
        const auto bytes=take(read_file_bytes(root/name));
        for(auto b:bytes) { hash^=std::to_integer<unsigned char>(b); hash*=1099511628211ull; }
    };
    for(const auto* name:{"pathtrace.slang","pathtrace-empty.slang","path-bsdf.slang","path-accumulate.slang","pathtrace-tone.slang","path-air.slang","path-air-empty.slang","path-cloud.slang"}) append(directory,name);
    if(restir) {
        // Stable relative names, never absolute build-machine paths. Cover the complete
        // private shader dependency closure, including the pinned SDK header contents.
        for(const auto& root:{directory,std::filesystem::path{DK_RTXDI_INCLUDE_DIR}}) {
            std::vector<std::string> names;
            for(const auto& file:std::filesystem::recursive_directory_iterator(root))
                if(file.is_regular_file()) names.push_back(file.path().lexically_relative(root).generic_string());
            std::sort(names.begin(),names.end());
            for(const auto& name:names) append(root,name);
        }
    }
    return std::format("fnv1a64:{:016x}",hash);
}
}
Result<JobQueue::Payload> render_path_capture(memory::ResourceHandle resource,const SceneReadSnapshot& input,
    const CaptureRequest& request,const CaptureInfo& info,render::ViewDescription view,const std::filesystem::path& output,
    std::stop_token stop,const JobQueue::Reporter& report) try {
    const auto start=std::chrono::steady_clock::now(); const auto& p=*request.path;
    auto progress=[&](std::string phase,std::uint64_t completed=0,std::uint64_t submitted=0) {
        report({std::move(phase),"spp",completed,submitted,p.target_spp}); checkpoint(stop);
    };
    progress("preparing");
    render::DiskSceneOptions options; options.stop=stop;
    auto disk=take(render::DiskScene::load_snapshot(resource,input.project,input.scene,options));
    std::vector<const CpuAsset*> assets; for(const auto& asset:disk.assets()) assets.push_back(asset.get());
    auto source=take(render::PathTraceScene::create(resource,disk.scene(),assets,{},render::PathTraceMaterialModel::metallic_roughness));
    graphics::DeviceOptions device_options; device_options.cancel=stop;
    device_options.ray_tracing=graphics::RayTracingMode::required;
    device_options.sampled_image_indexing=graphics::SampledImageIndexingMode::required;
    device_options.validation=request.require_validation ? graphics::ValidationMode::required : graphics::ValidationMode::if_available;
    auto device=take(graphics::Device::create(resource,device_options));
    auto queue=take(graphics::SubmissionQueue::create(resource,std::move(device)));
    auto completion=std::make_shared<Completion>(); completion->output=output;
    completion->provenance=nlohmann::json::parse(info.provenance);
    {
        progress("building");
        const std::filesystem::path shaders=DK_CAPTURE_PATH_SHADER_DIR;
        const auto shader=fingerprint(shaders,p.restir_di);
        auto renderer=take(render::PathTraceRenderer::create(resource,queue,shaders,stop));
        require(shader==fingerprint(shaders,p.restir_di),"Path shaders changed while compiling");
        auto gpu=take(renderer.prepare_scene(queue,source,stop));
        auto render_view=take(render::RenderView::create(disk.scene(),view));
        render::PathTraceSettings settings; settings.seed=p.seed; settings.max_bounces=p.max_bounces; settings.rr_start=p.rr_start;
        settings.environment_rgb=p.environment_rgb; settings.sun=p.sun ? render::PathTraceSun::scene : render::PathTraceSun::none;
        settings.surface_only=p.version>=2 || p.surface_only;
        render::PathTraceSchedule schedule; schedule.target_spp=p.target_spp; schedule.exposure=p.exposure_ev;
        render::RestirDiSettings restir;
        if(p.restir) {
            const auto& r=*p.restir; restir.version=r.version; restir.initial_candidates=r.initial_candidates; restir.max_candidate_evaluations=r.max_candidate_evaluations;
            if(r.temporal) { const auto& t=*r.temporal; restir.temporal=render::RestirTemporalSettings{t.version,t.max_history_candidates,t.max_history_age,t.position_tolerance,t.normal_cosine}; }
            if(r.spatial) { const auto& s=*r.spatial; restir.spatial=render::RestirSpatialSettings{s.version,s.neighbor_count,s.radius,s.max_source_candidates,s.position_tolerance,s.normal_cosine}; }
        }
        if(p.scene_media) {
            const auto& environment=input.scene.environment(); const auto& clouds=environment.clouds;
            render::CloudFieldCache fields; std::shared_ptr<const render::CloudFieldData> field;
            if(clouds.enabled && clouds.coverage>0 && clouds.density>0 && clouds.extinction_per_m>0) field=take(fields.prepare(clouds));
            checkpoint(stop);
            const auto medium=take(render::PathMediumSnapshot::create(environment,{1,p.time_seconds,p.cloud_albedo,p.cloud_anisotropy},field));
            render::VolumePathSettings volume; volume.seed=p.seed; volume.max_bounces=p.max_bounces; volume.rr_start=p.rr_start;
            volume.environment_rgb=p.environment_rgb; volume.sun=p.sun; volume.max_tracking_steps=p.max_tracking_steps; volume.max_steps_per_path=p.max_steps_per_path;
            check(p.restir_di ? renderer.configure_restir_medium_view(queue,1,gpu,medium,render_view,volume,restir,schedule) :
                renderer.configure_medium_view(queue,1,gpu,medium,render_view,volume,schedule));
        } else check(p.restir_di ? renderer.configure_restir_view(queue,1,gpu,render_view,settings,restir,schedule) :
            renderer.configure_view(queue,1,gpu,render_view,settings,schedule));
        std::uint32_t last_completed=UINT32_MAX,last_submitted=UINT32_MAX;
        for(;;) {
            checkpoint(stop); (void)take(renderer.poll(queue,stop));
            auto state=take(renderer.status(1));
            state.submitted_spp=std::max(state.completed_spp,state.submitted_spp);
            if(state.completed_spp!=last_completed || state.submitted_spp!=last_submitted) {
                progress("rendering",state.completed_spp,state.submitted_spp);
                last_completed=state.completed_spp; last_submitted=state.submitted_spp;
            }
            if(state.completed_spp==p.target_spp) break;
            (void)take(renderer.submit_batch(queue,1,stop));
            const auto pending=renderer.pending_submission();
            if(pending.value()) (void)take(queue.wait(pending,1000000));
        }
        progress("readback",p.target_spp,p.target_spp);
        auto completed=renderer.completed_frame(1);
        require(completed && completed.info().samples.first==0 && completed.info().samples.count==p.target_spp,"Path capture sample interval mismatch");
        require(completed.info().sampling_first_epoch==0 && !completed.info().temporal_history_epoch,"Capture did not start with empty sampling history");
        auto display=take(renderer.display(queue,completed,p.exposure_ev,true,stop));
        auto batch=take(queue.begin()); auto pixels=take(batch.readback(display.image,{0,0,0,0,request.width,request.height}));
        const auto display_readback=take(queue.submit(std::move(batch)));
        while(!take(queue.wait(display_readback,1000000))) checkpoint(stop);
        checkpoint(stop);
        ByteBuffer rgba(std::size_t{request.width}*request.height*4); require(take(pixels.try_read(rgba)),"Path display readback incomplete");
        auto& provenance=completion->provenance;
        provenance["shader_fingerprint"]=shader;
        provenance["input_fingerprint"]=std::format("fnv1a64:{:016x}",source.fingerprint()); provenance["actual_spp"]=p.target_spp;
        if(p.version>=2) provenance["geometry_fingerprint"]=std::format("fnv1a64:{:016x}",completed.info().geometry_fingerprint);
        if(p.scene_media) {
            require(completed.info().medium.has_value(),"Missing completed medium provenance");
            provenance["cloud_field_hash"]=std::format("fnv1a64:{:016x}",completed.info().medium->field_hash);
        }
        if(p.version==3 && p.restir_di) {
            const auto& used=completed.info(); require(used.restir.has_value(),"Missing completed ReSTIR provenance");
            const auto& t=used.temporal_statistics; const auto& s=used.spatial_statistics;
            provenance["reuse"]={{"temporal",{{"reused",t.reused},{"rejected",t.rejected},{"expired",t.expired},{"support_corrected",t.support_corrected},{"max_age",t.max_age},{"max_candidates",t.max_candidates}}},
                {"spatial",{{"reused",s.reused},{"rejected",s.rejected},{"support_corrected",s.support_corrected},{"current_visibility_rays",s.current_visibility_rays},{"max_age",s.max_age},{"max_candidates",s.max_candidates}}},
                {"prepared_surfaces",used.restir_medium_statistics.prepared_surfaces},{"applied_surfaces",used.restir_medium_statistics.applied_surfaces}};
        }
        if(request.linear_json) {
            auto readback=take(completed.read_linear(queue,stop)); std::optional<render::PathTraceGpuImage> image;
            while(!(image=take(readback.read(queue,stop)))) { checkpoint(stop); std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
            nlohmann::json linear=nlohmann::json::array();
            for(const auto& v:image->linear_rgb()) { require(v.allFinite(),"Nonfinite path HDR output"); linear.push_back({v.x(),v.y(),v.z()}); }
            std::vector<unsigned char> encoded; encoded.reserve(rgba.size()); for(auto b:rgba) encoded.push_back(std::to_integer<unsigned char>(b));
            const auto& adapter=queue.device().adapter(); const auto memory=renderer.memory_stats(queue);
            const auto text=nlohmann::json{{"schema_version",p.version==3 ? 2 : 1},{"kind","decker.pathtrace_capture"},{"status","succeeded"},
                {"document_id",info.document.to_string()},{"scene_id",info.frame.scene.to_string()},{"revision",info.frame.revision},
                {"frame",info.frame.frame},{"width",request.width},{"height",request.height},{"provenance",provenance},
                {"sample_first",0},{"sample_count",p.target_spp},{"linear_rgb",std::move(linear)},{"rgba8",std::move(encoded)},
                {"device",{{"name",adapter.properties.deviceName.data()},{"driver",adapter.driver.driverInfo.data()}}},
                {"build",DK_CAPTURE_BUILD},{"validation",queue.device().validation_enabled()},
                {"memory",{{"limit_bytes",memory.owned.limit},{"peak_bytes",memory.owned.peak}}},
                {"wall_ms",std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count()},
                {"gpu_ms",nullptr}}.dump();
            require(text.size()<=128U*1024U*1024U,"Path linear JSON exceeds 128 MiB");
            const auto bytes=std::as_bytes(std::span{text.data(),text.size()}); completion->ppm.assign(bytes.begin(),bytes.end());
        } else {
            const auto header="P6\n"+std::to_string(request.width)+" "+std::to_string(request.height)+"\n255\n";
            const auto bytes=std::as_bytes(std::span{header.data(),header.size()}); completion->ppm.assign(bytes.begin(),bytes.end());
            completion->ppm.reserve(bytes.size()+rgba.size()/4*3);
            for(std::size_t i=0;i<rgba.size();i+=4) completion->ppm.insert(completion->ppm.end(),rgba.begin()+i,rgba.begin()+i+3);
        }
        require(shader==fingerprint(shaders,p.restir_di),"Path shaders changed during capture");
    }
    check(queue.close());
    require(queue.device().validation_errors()==0 && queue.device().validation_warnings()==0,"Path capture Vulkan validation failed");
    completion->summary=nlohmann::json{{"kind","capture"},{"document_id",info.document.to_string()},
        {"scene_id",info.frame.scene.to_string()},{"revision",info.frame.revision},{"frame",info.frame.frame},
        {"width",request.width},{"height",request.height},{"output",info.output},{"draw_count",0},{"provenance",completion->provenance}}.dump();
    require(completion->summary.size()<=4096,"Path capture summary exceeds Job metadata limit");
    progress("publishing",p.target_spp,p.target_spp);
    return completion;
} catch(Error& e) { return std::unexpected(e.with_context("render.capture pathtrace")); }
}
