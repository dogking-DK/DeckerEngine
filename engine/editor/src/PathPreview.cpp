#include "PathPreview.hpp"
#include "EditorSupport.hpp"
#include <dk/render/DiskScene.hpp>
#include <dk/memory/Context.hpp>
#include <utility>
namespace dk::editor::detail {
PathPreview::PathPreview(memory::MemorySystem& memory,memory::ResourceHandle heap,graphics::Device device)
    : memory_(memory),heap_(heap),device_(std::move(device)),thread_([this](std::stop_token stop) { run(stop); }) {}
PathPreview::~PathPreview() { thread_.request_stop(); suspend(); condition_.notify_all(); thread_.join(); }
void PathPreview::request(PathPreviewRequest request) {
    std::lock_guard lock(mutex_); cancel_.request_stop(); cancel_=std::stop_source{};
    wanted_=request.sequence; enabled_=true; request_=std::move(request); result_.reset();
    status_.sequence=wanted_; status_.phase="Queued"; status_.error.clear(); condition_.notify_all();
}
void PathPreview::suspend() {
    std::lock_guard lock(mutex_); enabled_=false; cancel_.request_stop(); request_.reset(); result_.reset(); condition_.notify_all();
}
std::optional<PathPreviewResult> PathPreview::take_result() { std::lock_guard lock(mutex_); return std::exchange(result_,{}); }
PathPreviewStatus PathPreview::status() const { std::lock_guard lock(mutex_); return status_; }
void PathPreview::phase(std::uint64_t sequence,std::string value,const render::PathTraceProgress& progress) {
    std::lock_guard lock(mutex_); if(sequence==wanted_) { status_.phase=std::move(value); status_.progress=progress; }
}
void PathPreview::run(std::stop_token stop) {
    memory::ThreadContext context{memory_,heap_}; memory::ExecutionScope scope{context,heap_};
    std::optional<graphics::SubmissionQueue> queue;
    render::PathTraceRenderer renderer; bool initialized=false,configured=false,terminal=false,suspended=true,busy=false,pending_config=false;
    std::optional<PathPreviewRequest> active;
    std::optional<DocumentId> asset_session;
    std::uint64_t asset_reload=0,scene_revision=0,scene_preview=0,applied_reset=0;
    render::DiskScene disk; render::RenderScene scene; render::PathTraceGpuScene gpu;
    std::vector<PickMesh> meshes;
    std::optional<render::PathTraceDisplayFrame> display;
    std::optional<render::PathTraceFrame> linear;
    std::uint64_t display_sequence=0;
    render::PathTraceGpuImageInfo display_info;
    std::uint32_t displayed_spp=0;
    double displayed_exposure=0;
    std::stop_token cancel;
    for(;;) {
        std::optional<PathPreviewRequest> incoming; bool enabled;
        {
            std::unique_lock lock(mutex_);
            if(!enabled_ || !busy)
                condition_.wait_for(lock,stop,std::chrono::milliseconds(50),[&] { return request_.has_value(); });
            if(stop.stop_requested()) break;
            enabled=enabled_; incoming=std::exchange(request_,{});
            if(incoming) cancel=cancel_.get_token();
        }
        if(incoming) { active=std::move(incoming); displayed_spp=0; suspended=false; pending_config=true; }
        try {
            if(queue) { check(queue->poll()); if(configured) (void)take(renderer.poll(*queue)); }
            if(display && take(queue->wait(display->submission,0))) {
                if(active && enabled && display_sequence==active->sequence && !cancel.stop_requested()) {
                    PathPreviewResult result; result.sequence=display_sequence; result.document=active->snapshot.state;
                    result.camera=active->camera; result.info=display_info; result.scene=scene; result.meshes=meshes;
                    if(active->observe_memory) result.memory=renderer.memory_stats(*queue);
                    if(linear) {
                        auto value=take(linear->read(*queue,cancel));
                        if(!value) { busy=true; continue; }
                        result.linear=std::move(*value);
                    }
                    result.image=take(queue->export_image(display->image));
                    std::lock_guard lock(mutex_);
                    if(enabled_ && wanted_==display_sequence) result_=std::move(result);
                }
                display.reset();
                linear.reset();
            }
            if(!enabled) { if(configured && !suspended) check(renderer.cancel(1)); suspended=true; continue; }
            if(pending_config) {
                // Drain obsolete producer work by polling before rebuilding. This bounds the
                // queue even during continuous resize/drag and never blocks the GUI owner.
                if(queue && queue->stats().pending_slots) { busy=true; continue; }
                if(terminal) throw std::runtime_error("Path tracing device lost; restart the editor.");
                if(active->snapshot.scene.logical_bytes()>32U*1024U*1024U) throw std::runtime_error("Path preview snapshot exceeds 32 MiB");
                if(!queue) queue.emplace(take(graphics::SubmissionQueue::create(heap_,std::move(device_))));
                if(!initialized) {
                    phase(active->sequence,"Compiling");
                    renderer=take(render::PathTraceRenderer::create(heap_,*queue,DK_EDITOR_PATH_SHADER_DIR,cancel)); initialized=true;
                }
                const bool reload=asset_session!=active->snapshot.state.document_id || asset_reload!=active->reload;
                if(reload) {
                    phase(active->sequence,"Loading assets");
                    render::DiskSceneOptions options; options.stop=cancel; options.profile=GltfImportProfile::unlit_preview;
                    auto candidate=take(render::DiskScene::load_snapshot(heap_,active->snapshot.project,active->snapshot.scene,options));
                    std::vector<PickMesh> queries;
                    for(const auto& asset:candidate.assets()) {
                        if(cancel.stop_requested()) throw std::runtime_error("Path preview cancelled");
                        std::vector<geometry::Triangle> triangles;
                        for(const auto& primitive:asset->mesh.primitives) for(std::size_t i=0;i+2<primitive.indices.size();i+=3)
                            triangles.push_back({primitive.positions[primitive.indices[i]].cast<double>(),primitive.positions[primitive.indices[i+1]].cast<double>(),primitive.positions[primitive.indices[i+2]].cast<double>()});
                        if(triangles.size()>render::desktop_path_scene_limits().max_triangles) throw std::runtime_error("Path preview supports at most 1000000 triangles");
                        queries.push_back({asset->mesh.id,take(geometry::MeshQuery::build(triangles))});
                    }
                    disk=std::move(candidate); meshes=std::move(queries);
                }
                if(reload || !gpu || scene_revision!=active->snapshot.state.revision || scene_preview!=active->preview) {
                    phase(active->sequence,"Building scene");
                    std::optional<render::LocalTransformOverride> transform;
                    if(active->edit) transform=render::LocalTransformOverride{active->edit->entity,active->edit->value};
                    auto candidate=take(render::RenderScene::extract(heap_,active->snapshot.scene,transform));
                    std::vector<const CpuAsset*> assets; for(const auto& asset:disk.assets()) assets.push_back(asset.get());
                    auto source=take(render::PathTraceScene::create(heap_,candidate,assets,render::desktop_path_scene_limits(),render::PathTraceMaterialModel::metallic_roughness));
                    // The input is valid and obsolete submissions have drained. Keep the
                    // exported display image, but retire old tracing history/geometry before
                    // allocating a new large scene under the same renderer budget.
                    if(configured) { check(renderer.release_view(1)); configured=false; }
                    gpu={};
                    gpu=take(renderer.prepare_scene(*queue,source,cancel)); scene=std::move(candidate);
                    asset_session=active->snapshot.state.document_id; asset_reload=active->reload;
                    scene_revision=active->snapshot.state.revision; scene_preview=active->preview;
                }
                const auto view=take(render::RenderView::create(scene,{active->width,active->height,active->sequence,
                    active->camera.world(),active->camera.projection(double(active->width)/active->height)}));
                render::PathTraceSettings settings; settings.seed=active->settings.seed; settings.max_bounces=active->settings.bounces;
                settings.rr_start=std::min(3u,settings.max_bounces); settings.environment_rgb=active->settings.sky;
                settings.sun=active->settings.sun ? render::PathTraceSun::scene : render::PathTraceSun::none;
                settings.surface_only=active->settings.surface_only;
                render::PathTraceSchedule schedule; schedule.target_spp=active->settings.target; schedule.gesture=active->preview;
                schedule.paused=active->settings.paused; schedule.exposure=active->settings.exposure;
                check(renderer.configure_view(*queue,1,gpu,view,settings,schedule));
                if(configured && applied_reset!=active->settings.reset) check(renderer.reset(1));
                configured=true; applied_reset=active->settings.reset; pending_config=false;
            }
            if(!active || !configured || cancel.stop_requested()) continue;
            const auto progress=take(renderer.status(1));
            phase(active->sequence,progress.paused ? "Paused" : progress.completed_spp>=progress.target_spp ? "Complete" : "Rendering",progress);
            auto completed=renderer.completed_frame(1);
            if(!display && completed && (displayed_spp!=progress.completed_spp || displayed_exposure!=active->settings.exposure)) {
                display=take(renderer.display(*queue,completed,active->settings.exposure,!active->srgb,cancel));
                if(active->read_linear) linear=take(completed.read_linear(*queue,cancel));
                display_sequence=active->sequence; display_info=completed.info();
                displayed_spp=progress.completed_spp; displayed_exposure=active->settings.exposure;
            }
            if(!display) (void)take(renderer.submit_batch(*queue,1,cancel));
            busy=bool(display) || progress.pending || (!progress.paused && progress.completed_spp<progress.target_spp);
            // A Windows timed sleep can round a 1 ms tile delay up to a scheduler
            // tick. Wait for actual GPU completion instead; never hold mutex_ or
            // the shared queue gate, and check new requests at least every 1 ms.
            const auto ticket=linear ? linear->submission() : display ? display->submission : renderer.pending_submission();
            if(busy && ticket.value()) (void)take(queue->wait(ticket,1000000));
        } catch(const std::exception& error) {
            busy=false; pending_config=false;
            if(queue && queue->stats().device_lost) terminal=true;
            if(active) {
                std::lock_guard lock(mutex_);
                if(enabled_ && wanted_==active->sequence && !cancel.stop_requested()) { status_.phase="Failed"; status_.error=error.what(); enabled_=false; }
            }
        }
    }
    if(queue) { if(configured) (void)renderer.cancel(1); (void)queue->close(); }
}
}
