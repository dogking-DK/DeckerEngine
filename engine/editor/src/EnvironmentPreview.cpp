#include "EnvironmentPreview.hpp"
#include "EditorSupport.hpp"
#include <utility>
#include <dk/render/DiskScene.hpp>
#include <dk/memory/Context.hpp>
namespace dk::editor::detail {
EnvironmentPreview::EnvironmentPreview(memory::MemorySystem& memory,memory::ResourceHandle heap,const graphics::DeviceOptions& options,bool profile_gpu)
    : memory_(memory),heap_(heap),options_(options),profile_gpu_(profile_gpu),thread_([this](std::stop_token stop) { run(stop); }) {}
EnvironmentPreview::~EnvironmentPreview() { thread_.request_stop(); condition_.notify_all(); thread_.join(); }
void EnvironmentPreview::request(EnvironmentPreviewRequest request) {
    std::lock_guard lock(mutex_); wanted_=request.sequence; request_=std::move(request); result_.reset(); condition_.notify_all();
}
void EnvironmentPreview::cancel() { std::lock_guard lock(mutex_); wanted_=0; request_.reset(); result_.reset(); invalidate_history_=true; condition_.notify_all(); }
bool EnvironmentPreview::current(std::uint64_t sequence) const { std::lock_guard lock(mutex_); return sequence==wanted_; }
std::uint64_t EnvironmentPreview::active_sequence() const { std::lock_guard lock(mutex_); return active_; }
std::uint64_t EnvironmentPreview::discarded() const { std::lock_guard lock(mutex_); return discarded_; }
std::optional<EnvironmentPreviewResult> EnvironmentPreview::take_result() { std::lock_guard lock(mutex_); return std::exchange(result_,{}); }
void EnvironmentPreview::run(std::stop_token stop) {
    memory::ThreadContext context{memory_,heap_}; memory::ExecutionScope scope{context,heap_};
    std::optional<graphics::SubmissionQueue> owned_queue;
    render::ScenePipeline pipeline; bool ready=false;
    render::GpuAssets assets;
    std::optional<DocumentId> session;
    std::vector<PickMesh> meshes;
    bool previous_edit=false,reset_history=false;
    bool history_refined=false;
    std::optional<Mat4d> previous_camera;
    for(;;) {
        std::optional<EnvironmentPreviewRequest> request;
        {
            std::unique_lock lock(mutex_);
            condition_.wait(lock,stop,[&] { return request_.has_value(); });
            if(stop.stop_requested()) break;
            request=std::exchange(request_,{}); active_=request->sequence;
            reset_history=std::exchange(invalidate_history_,false) || reset_history;
        }
        EnvironmentPreviewResult result; result.sequence=request->sequence; bool published=false;
        try {
            check(request->camera.validate(double(request->width)/request->height));
            if(request->snapshot.scene.logical_bytes()>32U*1024U*1024U) throw std::runtime_error("Environment preview snapshot exceeds 32 MiB");
            const auto& atmosphere=request->snapshot.scene.environment().atmosphere;
            const Vec3d center{atmosphere.planet_center_m[0],atmosphere.planet_center_m[1],atmosphere.planet_center_m[2]};
            if((request->camera.world().matrix().topRightCorner<3,1>()-center).stableNorm()<atmosphere.ground_radius_m)
                throw std::runtime_error("Camera is below the atmosphere ground. Move upward or press Home in the viewport to reset.");
            if(!ready) {
                options_.secondary_queue=false;
                owned_queue.emplace(take(graphics::SubmissionQueue::create(heap_,take(graphics::Device::create(heap_,options_)))));
                if(profile_gpu_) check(owned_queue->configure_gpu_profiling({true}));
                pipeline=take(render::ScenePipeline::create(heap_,*owned_queue,DK_EDITOR_SHADER_DIR)); ready=true;
            }
            auto& queue=*owned_queue;
            if(session!=request->snapshot.state.document_id) {
                reset_history=true;
                auto source=take(render::DiskScene::load_snapshot(heap_,request->snapshot.project,request->snapshot.scene,{GltfImportProfile::unlit_preview}));
                std::vector<PickMesh> queries;
                for(const auto& asset:source.assets()) {
                    std::vector<geometry::Triangle> triangles;
                    for(const auto& p:asset->mesh.primitives) for(std::size_t i=0;i+2<p.indices.size();i+=3)
                        triangles.push_back({p.positions[p.indices[i]].cast<double>(),p.positions[p.indices[i+1]].cast<double>(),p.positions[p.indices[i+2]].cast<double>()});
                    queries.push_back({asset->mesh.id,take(geometry::MeshQuery::build(triangles))});
                }
                auto candidate=take(source.upload(queue)); assets=std::move(candidate); meshes=std::move(queries); session=request->snapshot.state.document_id;
            }
            if(!stop.stop_requested() && current(request->sequence)) {
                std::optional<render::LocalTransformOverride> transform;
                if(request->edit) transform=render::LocalTransformOverride{request->edit->entity,request->edit->value};
                result.scene=take(render::RenderScene::extract(heap_,request->snapshot.scene,transform));
                auto view=take(render::RenderView::create(result.scene,{request->width,request->height,request->sequence,
                    request->camera.world(),request->camera.projection(double(request->width)/request->height)}));
                auto settings=render::unlit_preview_settings(); settings.exposure=request->exposure;
                result.environment=request->environment;
                const auto& clouds=request->snapshot.scene.environment().clouds;
                const bool cloudy=clouds.enabled && clouds.coverage>0 && clouds.density>0 && clouds.extinction_per_m>0;
                result.warmup_frames=cloudy && request->environment.mode==render::CloudRenderMode::temporal ? 32u : 0u;
                // Detached Gizmo overrides do not advance the Scene revision.
                // Repeated stationary owner edits must reproduce a standalone capture.
                // Only actual navigation reprojects across requests; a partial cold history
                // remains eligible for reprojection, but not for the shorter refinement budget.
                const auto camera_world=request->camera.world().matrix();
                const bool stationary=previous_camera && (previous_camera->array()==camera_world.array()).all();
                if((reset_history || previous_edit || request->edit || stationary) && pipeline.cloud_history(request->environment.view_id))
                    check(pipeline.release_cloud_history(queue,request->environment.view_id));
                reset_history=false;
                previous_edit=request->edit.has_value();
                bool reused=false;
                for(unsigned i=0;i<=result.warmup_frames;++i) {
                    if(i==1) {
                        // Give the owner time to coalesce motion after publishing the first image.
                        std::unique_lock lock(mutex_);
                        condition_.wait_for(lock,stop,std::chrono::milliseconds(50),[&] { return wanted_!=request->sequence; });
                    }
                    if(stop.stop_requested() || !current(request->sequence)) break;
                    auto frame=take(pipeline.render_environment(queue,view,assets,settings,request->environment));
                    if(!take(frame.wait(queue))) throw std::runtime_error("Environment preview did not complete");
                    if(i==0) {
                        reused=frame.cloud_optimization().history_available;
                        if(!reused) history_refined=false;
                        if(reused && history_refined) result.warmup_frames=8;
                        previous_camera=camera_world;
                    }
                    if(i==result.warmup_frames) history_refined=true;
                    if(i%4==0 || i==result.warmup_frames) {
                        EnvironmentPreviewResult update; update.sequence=result.sequence; update.scene=result.scene;
                        update.environment=request->environment; update.warmup_frames=i;
                        update.refinement_target=result.warmup_frames; update.refinement_complete=i==result.warmup_frames;
                        update.history_reused=reused;
                        update.info=frame.info(); update.pixels.resize(std::size_t(request->width)*request->height*4);
                        check(frame.read_rgba8(update.pixels)); update.meshes=meshes;
                        update.history=frame.cloud_optimization(); update.shadow_generation=frame.cloud_shadow_generation();
                        if(profile_gpu_) {
                            update.gpu_profile=frame.submission().gpu_profile();
                            VmaTotalStatistics stats{}; vmaCalculateStatistics(queue.device().allocator(),&stats);
                            const VkPhysicalDeviceMemoryProperties* properties=nullptr; vmaGetMemoryProperties(queue.device().allocator(),&properties);
                            for(unsigned type=0;type<properties->memoryTypeCount;++type)
                                if(properties->memoryTypes[type].propertyFlags&VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
                                    update.device_local_bytes+=stats.memoryType[type].statistics.allocationBytes;
                        }
                        std::lock_guard lock(mutex_);
                        if(!stop.stop_requested() && wanted_==request->sequence) { result_=std::move(update); published=true; }
                    }
                }
            }
        } catch(const std::exception& error) { result.error=error.what(); }
        {
            std::lock_guard lock(mutex_);
            if(!stop.stop_requested() && wanted_==request->sequence) {
                if(!published || !result.error.empty()) result_=std::move(result);
            }
            else ++discarded_;
            active_=0;
        }
    }
    // Queue destruction drains submitted work before releasing retained owners.
}
}
