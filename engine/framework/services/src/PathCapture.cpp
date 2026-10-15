#include "CaptureInternal.hpp"
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
std::string fingerprint(const std::filesystem::path& directory) {
    std::uint64_t hash=14695981039346656037ull;
    for(const auto* name:{"pathtrace.slang","pathtrace-empty.slang","path-bsdf.slang","path-accumulate.slang","pathtrace-tone.slang"}) {
        for(char c:std::string_view{name}) { hash^=static_cast<unsigned char>(c); hash*=1099511628211ull; }
        const auto bytes=take(read_file_bytes(directory/name));
        for(auto b:bytes) { hash^=std::to_integer<unsigned char>(b); hash*=1099511628211ull; }
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
        const auto shader=fingerprint(shaders);
        auto renderer=take(render::PathTraceRenderer::create(resource,queue,shaders,stop));
        require(shader==fingerprint(shaders),"Path shaders changed while compiling");
        auto gpu=take(renderer.prepare_scene(queue,source,stop));
        auto render_view=take(render::RenderView::create(disk.scene(),view));
        render::PathTraceSettings settings; settings.seed=p.seed; settings.max_bounces=p.max_bounces; settings.rr_start=p.rr_start;
        settings.environment_rgb=p.environment_rgb; settings.sun=p.sun ? render::PathTraceSun::scene : render::PathTraceSun::none;
        settings.surface_only=p.surface_only;
        render::PathTraceSchedule schedule; schedule.target_spp=p.target_spp; schedule.exposure=p.exposure_ev;
        check(renderer.configure_view(queue,1,gpu,render_view,settings,schedule));
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
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        progress("readback",p.target_spp,p.target_spp);
        auto completed=renderer.completed_frame(1);
        require(completed && completed.info().samples.first==0 && completed.info().samples.count==p.target_spp,"Path capture sample interval mismatch");
        auto display=take(renderer.display(queue,completed,p.exposure_ev,true,stop));
        auto batch=take(queue.begin()); auto pixels=take(batch.readback(display.image,{0,0,0,0,request.width,request.height}));
        const auto display_readback=take(queue.submit(std::move(batch)));
        while(!take(queue.wait(display_readback,1000000))) checkpoint(stop);
        checkpoint(stop);
        ByteBuffer rgba(std::size_t{request.width}*request.height*4); require(take(pixels.try_read(rgba)),"Path display readback incomplete");
        auto& provenance=completion->provenance;
        provenance["shader_fingerprint"]=shader;
        provenance["input_fingerprint"]=std::format("fnv1a64:{:016x}",source.fingerprint()); provenance["actual_spp"]=p.target_spp;
        if(request.linear_json) {
            auto readback=take(completed.read_linear(queue,stop)); std::optional<render::PathTraceGpuImage> image;
            while(!(image=take(readback.read(queue,stop)))) { checkpoint(stop); std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
            nlohmann::json linear=nlohmann::json::array();
            for(const auto& v:image->linear_rgb()) { require(v.allFinite(),"Nonfinite path HDR output"); linear.push_back({v.x(),v.y(),v.z()}); }
            std::vector<unsigned char> encoded; encoded.reserve(rgba.size()); for(auto b:rgba) encoded.push_back(std::to_integer<unsigned char>(b));
            const auto& adapter=queue.device().adapter(); const auto memory=renderer.memory_stats(queue);
            const auto text=nlohmann::json{{"schema_version",1},{"kind","decker.pathtrace_capture"},{"status","succeeded"},
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
        require(shader==fingerprint(shaders),"Path shaders changed during capture");
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
