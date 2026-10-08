#include <dk/services/CaptureService.hpp>
#include <dk/render/DiskScene.hpp>
#include <dk/memory/MemorySystem.hpp>
#include <dk/io/File.hpp>
#include <dk/profiling/Profiler.hpp>
#include <nlohmann/json.hpp>
#include <cmath>
#include <numbers>

namespace dk {
namespace {
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
    const double f=1/std::tan(c.fov_y*std::numbers::pi/360);
    v.projection=Mat4d::Zero(); v.projection(0,0)=f/(static_cast<double>(r.width)/r.height); v.projection(1,1)=-f;
    v.projection(2,2)=c.far_plane/(c.near_plane-c.far_plane);
    v.projection(2,3)=c.far_plane*c.near_plane/(c.near_plane-c.far_plane); v.projection(3,2)=-1;
    require(v.projection.allFinite(),"Capture projection overflow");
    return v;
}
struct Completion { std::filesystem::path output; ByteBuffer ppm; std::string summary; };
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
            auto pipeline=take(render::ScenePipeline::create(resource,queue,shader_directory)); checkpoint(stop);
            auto frame=take(pipeline.render(queue,take(render::RenderView::create(source.scene(),view)),assets,render::unlit_preview_settings()));
            require(take(frame.wait(queue)),"Capture GPU wait did not complete",ErrorCode::invalid_state);
            checkpoint(stop);
            frame_info=frame.info();
            ByteBuffer rgba(static_cast<std::size_t>(request.width)*request.height*4); check(frame.read_rgba8(rgba));
            const std::string header="P6\n"+std::to_string(request.width)+" "+std::to_string(request.height)+"\n255\n";
            const auto bytes=std::as_bytes(std::span{header.data(),header.size()});
            completion->ppm.reserve(bytes.size()+rgba.size()/4*3);
            completion->ppm.insert(completion->ppm.end(),bytes.begin(),bytes.end());
            for (std::size_t i=0;i<rgba.size();i+=4) completion->ppm.insert(completion->ppm.end(),rgba.begin()+i,rgba.begin()+i+3);
        }
        check(queue.close());
        require(queue.device().validation_errors()==0,"Capture Vulkan validation failed",ErrorCode::internal_error);
        checkpoint(stop);
        completion->summary=nlohmann::json{{"kind","capture"},{"document_id",info.document.to_string()},
            {"scene_id",frame_info.scene.to_string()},{"revision",frame_info.revision},{"frame",frame_info.frame},
            {"width",frame_info.width},{"height",frame_info.height},{"output",info.output},
            {"draw_count",frame_info.draw_count}}.dump();
        require(completion->summary.size()<=4096,"Capture summary exceeds Job metadata limit");
        return completion;
    } catch (Error& e) { return std::unexpected(e.with_context("render.capture")); }
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
        require(request.profile==GltfImportProfile::strict || request.profile==GltfImportProfile::unlit_preview,"Invalid capture profile");
        require(!request.output.empty() && request.output.size()<=1024,"Capture output must be 1..1024 UTF-8 bytes");
        auto relative=take(path_from_utf8(request.output));
        require(relative.extension()==".ppm","Capture output must have .ppm extension");
        auto snapshot=take(scene.read_snapshot(guard));
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
        input_bytes+=static_cast<std::size_t>(request.width)*request.height*7;
        require(s.next_frame!=UINT64_MAX,"Capture frame counter exhausted",ErrorCode::invalid_state);
        view.frame=s.next_frame;
        CaptureInfo info{snapshot.state.document_id,{snapshot.scene.id(),snapshot.scene.revision(),view.frame,request.width,request.height,0},output_name};
        auto job=s.queue.submit([input=std::move(snapshot),request,info,view,resource=s.render_heap,shaders=s.shaders,output](std::stop_token stop) {
            return render_capture(resource,input,request,info,view,shaders,output,stop);
        },input_bytes);
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
