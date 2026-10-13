#include "EnvironmentPreview.hpp"
#include "EditorSupport.hpp"
#include <utility>
#include <dk/render/DiskScene.hpp>
#include <dk/memory/Context.hpp>
namespace dk::editor::detail {
EnvironmentPreview::EnvironmentPreview(memory::MemorySystem& memory,memory::ResourceHandle heap,const graphics::DeviceOptions& options)
    : memory_(memory),heap_(heap),options_(options),thread_([this](std::stop_token stop) { run(stop); }) {}
EnvironmentPreview::~EnvironmentPreview() { thread_.request_stop(); condition_.notify_all(); thread_.join(); }
void EnvironmentPreview::request(EnvironmentPreviewRequest request) {
    std::lock_guard lock(mutex_); wanted_=request.sequence; request_=std::move(request); result_.reset(); condition_.notify_all();
}
void EnvironmentPreview::cancel() { std::lock_guard lock(mutex_); wanted_=0; request_.reset(); result_.reset(); }
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
    for(;;) {
        std::optional<EnvironmentPreviewRequest> request;
        {
            std::unique_lock lock(mutex_);
            condition_.wait(lock,stop,[&] { return request_.has_value(); });
            if(stop.stop_requested()) break;
            request=std::exchange(request_,{}); active_=request->sequence;
        }
        EnvironmentPreviewResult result; result.sequence=request->sequence;
        try {
            if(request->snapshot.scene.logical_bytes()>32U*1024U*1024U) throw std::runtime_error("Environment preview snapshot exceeds 32 MiB");
            if(!ready) {
                options_.secondary_queue=false;
                owned_queue.emplace(take(graphics::SubmissionQueue::create(heap_,take(graphics::Device::create(heap_,options_)))));
                pipeline=take(render::ScenePipeline::create(heap_,*owned_queue,DK_EDITOR_SHADER_DIR)); ready=true;
            }
            auto& queue=*owned_queue;
            if(session!=request->snapshot.state.document_id) {
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
                auto frame=take(pipeline.render_environment(queue,view,assets,settings));
                if(!take(frame.wait(queue))) throw std::runtime_error("Environment preview did not complete");
                result.info=frame.info(); result.pixels.resize(std::size_t(request->width)*request->height*4);
                check(frame.read_rgba8(result.pixels)); result.meshes=meshes;
            }
        } catch(const std::exception& error) { result.error=error.what(); }
        {
            std::lock_guard lock(mutex_);
            if(!stop.stop_requested() && wanted_==request->sequence) result_=std::move(result);
            else ++discarded_;
            active_=0;
        }
    }
    // Queue destruction drains submitted work before releasing retained owners.
}
}
