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
    std::uint64_t asset_reload=0,scene_revision=0,scene_preview=0,applied_reset=0,session=0;
    std::shared_ptr<const render::CloudFieldData> cloud_field;
    render::DiskScene disk; render::RenderScene scene; render::PathTraceGpuScene gpu;
    std::vector<PickMesh> meshes;
    std::optional<render::PathTraceDisplayFrame> display;
    std::optional<render::PathTraceFrame> linear;
    std::uint64_t display_sequence=0;
    render::PathTraceGpuImageInfo display_info;
    std::uint32_t displayed_spp=0;
    double displayed_exposure=0;
    std::stop_token cancel;
    std::optional<PathPreviewSettings> applied_settings;
    std::uint32_t applied_width=0,applied_height=0;
    Camera applied_camera;
    for(;;) {
        std::optional<PathPreviewRequest> incoming; bool enabled;
        {
            std::unique_lock lock(mutex_);
            if(!busy)
                condition_.wait_for(lock,stop,std::chrono::milliseconds(50),[&] { return request_.has_value() || (!enabled_ && !suspended); });
            if(stop.stop_requested()) break;
            enabled=enabled_; incoming=std::exchange(request_,{});
            if(incoming) cancel=cancel_.get_token();
        }
        if(incoming) { active=std::move(incoming); displayed_spp=0; suspended=false; pending_config=true; }
        try {
            if(queue) { check(queue->poll()); if(configured) (void)take(renderer.poll(*queue)); }
            if(display && take(queue->wait(display->submission,0))) {
                // Keep the readback ticket even for an obsolete/cancelled image:
                // retiring it early would leave a pending queue slot with no
                // ticket for the reconfiguration/suspend drain to wait on.
                if(linear && !take(queue->wait(linear->submission(),0))) {
                    busy=true; (void)take(queue->wait(linear->submission(),1000000)); continue;
                }
                if(active && enabled && display_sequence==active->sequence && !cancel.stop_requested()) {
                    PathPreviewResult result; result.sequence=display_sequence; result.document=active->snapshot.state;
                    result.camera=active->camera; result.info=display_info; result.scene=scene; result.meshes=meshes;
                    if(active->observe_memory) result.memory=renderer.memory_stats(*queue);
                    if(linear) {
                        auto value=take(linear->read(*queue,cancel));
                        if(!value) {
                            busy=true; (void)take(queue->wait(linear->submission(),1000000)); continue;
                        }
                        result.linear=std::move(*value);
                    }
                    result.image=take(queue->export_image(display->image));
                    std::lock_guard lock(mutex_);
                    if(enabled_ && wanted_==display_sequence) result_=std::move(result);
                }
                display.reset();
                linear.reset();
            }
            if(!enabled) {
                if(configured && !suspended) check(renderer.cancel(1));
                suspended=true; busy=queue && queue->stats().pending_slots!=0;
                if(!busy && active) phase(active->sequence,"Suspended");
                else if(busy) {
                    const auto ticket=display ? display->submission : renderer.pending_submission();
                    if(ticket.value()) (void)take(queue->wait(ticket,1000000));
                }
                continue;
            }
            if(pending_config) {
                // Drain obsolete producer work by polling before rebuilding. This bounds the
                // queue even during continuous resize/drag and never blocks the GUI owner.
                if(queue && queue->stats().pending_slots) {
                    busy=true;
                    const auto ticket=linear ? linear->submission() : display ? display->submission : renderer.pending_submission();
                    if(ticket.value()) (void)take(queue->wait(ticket,1000000));
                    continue;
                }
                if(terminal) throw std::runtime_error("Path tracing device lost; restart the editor.");
                if(active->snapshot.scene.logical_bytes()>32U*1024U*1024U) throw std::runtime_error("Path preview snapshot exceeds 32 MiB");
                if(!queue) queue.emplace(take(graphics::SubmissionQueue::create(heap_,std::move(device_))));
                if(!initialized) {
                    phase(active->sequence,"Compiling");
                    const auto limit=render::PathTraceRenderer::recommended_memory_limit(queue->device());
                    renderer=take(render::PathTraceRenderer::create(heap_,*queue,DK_EDITOR_PATH_SHADER_DIR,cancel,limit)); initialized=true;
                    std::lock_guard lock(mutex_); status_.gpu_memory_limit=limit;
                }
                const auto old_generation=configured ? take(renderer.status(1)).generation : 0;
                std::string reset_reason=!configured ? "New view" : asset_session!=active->snapshot.state.document_id ? "Document changed" :
                    asset_reload!=active->reload ? "Assets refreshed" : scene_preview!=active->preview ? "Edit gesture" :
                    scene_revision!=active->snapshot.state.revision ? "Scene changed" :
                    applied_width!=active->width || applied_height!=active->height ? "View size changed" :
                    applied_reset!=active->settings.reset ? "Manual reset" :
                    applied_settings && (applied_settings->restir_di!=active->settings.restir_di || applied_settings->scene_media!=active->settings.scene_media) ? "Algorithm or medium changed" :
                    applied_settings && (applied_settings->restir_candidates!=active->settings.restir_candidates || applied_settings->restir_temporal!=active->settings.restir_temporal || applied_settings->restir_spatial!=active->settings.restir_spatial) ? "ReSTIR configuration changed" :
                    applied_camera.projection_mode()!=active->camera.projection_mode() || applied_camera.settings().lens!=active->camera.settings().lens ? "Projection changed" :
                    applied_camera.world().matrix()!=active->camera.world().matrix() ? "Camera pose changed" : "Transport settings or resumed view";
                const bool reload=asset_session!=active->snapshot.state.document_id || asset_reload!=active->reload;
                if(reload) {
                    phase(active->sequence,"Loading assets");
                    render::DiskSceneOptions options; options.stop=cancel; options.profile=GltfImportProfile::unlit_preview;
                    const auto capacity=std::size_t(renderer.memory_stats(*queue).owned.limit);
                    options.max_assets=std::numeric_limits<std::uint32_t>::max(); options.cpu_bytes=capacity;
                    auto& limits=options.import_limits;
                    limits.source_bytes=limits.dependency_bytes=limits.input_bytes=limits.output_bytes=capacity;
                    limits.image_bytes=limits.texture_bytes=capacity;
                    limits.vertices=limits.indices=limits.primitives=std::numeric_limits<std::uint32_t>::max();
                    limits.image_dimension=queue->device().adapter().properties.limits.maxImageDimension2D;
                    auto candidate=take(render::DiskScene::load_snapshot(heap_,active->snapshot.project,active->snapshot.scene,options));
                    std::vector<PickMesh> queries;
                    for(const auto& asset:candidate.assets()) {
                        if(cancel.stop_requested()) throw std::runtime_error("Path preview cancelled");
                        std::vector<geometry::Triangle> triangles;
                        for(const auto& primitive:asset->mesh.primitives) for(std::size_t i=0;i+2<primitive.indices.size();i+=3)
                            triangles.push_back({primitive.positions[primitive.indices[i]].cast<double>(),primitive.positions[primitive.indices[i+1]].cast<double>(),primitive.positions[primitive.indices[i+2]].cast<double>()});
                        if(triangles.size()>render::desktop_path_scene_limits().max_triangles) throw std::runtime_error("Path geometry exceeds shader address capacity");
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
                    auto rebound=!reload && gpu ? gpu.source().rebind(heap_,candidate) : Result<render::PathTraceScene>{std::unexpected(Error{ErrorCode::conflict,"Asset reload"})};
                    if(!rebound && rebound.error().code!=ErrorCode::conflict) throw std::runtime_error(rebound.error().message);
                    auto source=rebound ? std::move(*rebound) : take(render::PathTraceScene::create(heap_,candidate,assets,render::desktop_path_scene_limits(),render::PathTraceMaterialModel::metallic_roughness));
                    // The input is valid and obsolete submissions have drained. Keep the
                    // exported display image, but retire old tracing history/geometry before
                    // allocating a new large scene under the same renderer budget.
                    // Keep geometry alive across environment-only edits so prepare_scene can
                    // verify consumed content and reuse AS. Large geometry changes still retire first.
                    if(!gpu || gpu.source().geometry_fingerprint()!=source.geometry_fingerprint()) {
                        if(configured) { check(renderer.release_view(1)); configured=false; }
                        gpu={};
                    }
                    gpu=take(renderer.prepare_scene(*queue,source,cancel)); scene=std::move(candidate);
                    if(reload) ++session; // A new document or explicit asset refresh starts fresh history.
                    asset_session=active->snapshot.state.document_id; asset_reload=active->reload;
                    scene_revision=active->snapshot.state.revision; scene_preview=active->preview;
                }
                check(active->camera.validate(double(active->width)/active->height));
                const auto view=take(render::RenderView::create(scene,{active->width,active->height,active->sequence,
                    active->camera.world(),active->camera.projection(double(active->width)/active->height)}));
                render::PathTraceSettings settings; settings.seed=active->settings.seed; settings.max_bounces=active->settings.bounces;
                settings.rr_start=std::min(3u,settings.max_bounces); settings.environment_rgb=active->settings.sky;
                settings.sun=active->settings.sun ? render::PathTraceSun::scene : render::PathTraceSun::none;
                settings.surface_only=active->settings.surface_only;
                render::PathTraceSchedule schedule; schedule.session=session; schedule.target_spp=active->settings.target; schedule.gesture=active->preview;
                schedule.tile_edge=256;
                schedule.tiles_per_submission=4;
                schedule.paused=active->settings.paused; schedule.exposure=active->settings.exposure;
                render::RestirDiSettings restir; restir.initial_candidates=active->settings.restir_candidates;
                if(active->settings.restir_temporal) restir.temporal.emplace();
                if(active->settings.restir_spatial) restir.spatial.emplace();
                if(active->settings.scene_media) {
                    const auto& environment=active->snapshot.scene.environment(); const auto& clouds=environment.clouds;
                    std::shared_ptr<const render::CloudFieldData> candidate;
                    if(clouds.enabled && clouds.coverage>0 && clouds.density>0 && clouds.extinction_per_m>0) {
                        if(cloud_field && cloud_field->key()==render::cloud_field_key(clouds)) candidate=cloud_field;
                        else {
                            // Two CPU generations at most: committed plus local candidate.
                            render::CloudFieldCache fields{{8U*1024U*1024U-(cloud_field ? cloud_field->bytes() : 0),1}};
                            candidate=take(fields.prepare(clouds));
                        }
                    }
                    if(cancel.stop_requested()) throw std::runtime_error("Path preview cancelled");
                    const auto medium=take(render::PathMediumSnapshot::create(environment,
                        {1,active->settings.time_seconds,active->settings.cloud_albedo,active->settings.cloud_anisotropy},candidate));
                    render::VolumePathSettings volume; volume.seed=settings.seed; volume.max_bounces=settings.max_bounces;
                    volume.rr_start=settings.rr_start; volume.environment_rgb=settings.environment_rgb; volume.sun=active->settings.sun;
                    check(active->settings.restir_di ? renderer.configure_restir_medium_view(*queue,1,gpu,medium,view,volume,restir,schedule) :
                        renderer.configure_medium_view(*queue,1,gpu,medium,view,volume,schedule));
                    cloud_field=std::move(candidate);
                } else { check(active->settings.restir_di ? renderer.configure_restir_view(*queue,1,gpu,view,settings,restir,schedule) :
                    renderer.configure_view(*queue,1,gpu,view,settings,schedule)); cloud_field.reset(); }
                if(configured && applied_reset!=active->settings.reset) check(renderer.reset(1));
                if(!configured || take(renderer.status(1)).generation!=old_generation) {
                    std::lock_guard lock(mutex_); if(wanted_==active->sequence) status_.reset_reason=std::move(reset_reason);
                }
                applied_settings=active->settings; applied_camera=active->camera; applied_width=active->width; applied_height=active->height;
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
            // Tone and acceptance readback only read the immutable completed HDR;
            // the next batch writes a separate candidate. Three queue slots cover
            // tone, optional readback and one sampling submission without a CPU gap.
            if(!renderer.pending_submission().value() && take(renderer.submit_batch(*queue,1,cancel)))
                phase(active->sequence,"Rendering",take(renderer.status(1)));
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
