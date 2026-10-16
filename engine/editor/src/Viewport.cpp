#include "Viewport.hpp"
#include "EditorSupport.hpp"
#include <imgui_impl_vulkan.h>
#include <cmath>
#include <algorithm>
#include <vector>

namespace dk::editor::detail {
Viewport::Viewport(memory::MemorySystem& memory,memory::ResourceHandle heap,graphics::SubmissionQueue& queue,const graphics::DeviceOptions& options,bool retain_pixels)
    : heap_(heap),queue_(queue),pipeline_(take(render::ScenePipeline::create(heap,queue,DK_EDITOR_SHADER_DIR))),environment_(memory,heap,options),retain_pixels_(retain_pixels) {
#ifdef DK_EDITOR_PATHTRACING
    if(!queue.device().ray_tracing_enabled()) path_unavailable_="GPU ray tracing is unavailable (AS / RT pipeline / buffer device address).";
    else if(!queue.device().sampled_image_indexing_enabled()) path_unavailable_="Non-uniform sampled image indexing is unavailable.";
    else { path_=std::make_unique<PathPreview>(memory,heap,take(queue.device().share_queue(0))); path_unavailable_.clear(); }
#endif
}
Viewport::~Viewport() { close_path(); release_texture(); }
void Viewport::close_path() {
#ifdef DK_EDITOR_PATHTRACING
    path_.reset();
#endif
}
void Viewport::suspend_path() {
#ifdef DK_EDITOR_PATHTRACING
    if(path_active_) { path_->suspend(); path_active_=false; attempted_.reset(); path_settings_.reset(); }
#endif
}
void Viewport::release_texture() {
    if (descriptor_) ImGui_ImplVulkan_RemoveTexture(descriptor_);
    descriptor_=VK_NULL_HANDLE;
    attempted_.reset();
    simulation_view_.reset();
}
void Viewport::update(const SceneReadSnapshot& snapshot,std::uint32_t width,std::uint32_t height,bool srgb,const Camera& camera,
    const std::optional<TransformEdit>& edit,std::uint64_t preview_revision,float exposure,const render::EnvironmentRenderSettings& environment) {
    suspend_path(); show_edit();
    if(auto valid=camera.validate(double(width)/height); !valid) {
        environment_.cancel(); environment_key_.reset(); environment_has_frame_=true;
        attempted_.reset(); published_.reset(); error_=valid.error().message; return;
    }
    const Key key{snapshot.state.document_id,snapshot.state.revision,width,height,srgb,camera.revision(),preview_revision,exposure,environment};
    if(snapshot.scene.environment().enabled || snapshot.scene.environment().clouds.enabled) {
        try {
            if(auto result=environment_.take_result(); result && result->sequence==environment_sequence_) {
                environment_has_frame_=true;
                // A continuously moving camera must still get completed images.
                // Keep their actual camera/key for picking; scene edits are never
                // published against a newer document or different render settings.
                auto compatible=key;
                if(environment_key_) {
                    compatible.camera=environment_key_->camera; compatible.width=environment_key_->width; compatible.height=environment_key_->height;
                    compatible.environment.camera_revision=environment_key_->environment.camera_revision;
                }
                if(environment_key_ && *environment_key_==compatible) {
                    if(!result->error.empty()) {
                        if(*environment_key_==key) error_=result->error;
                    } else {
                        publish(std::move(result->pixels),result->info,std::move(result->scene),environment_camera_,*environment_key_);
                        meshes_=std::move(result->meshes); published_environment_=result->environment; published_history_=result->history;
                        published_warmup_=result->warmup_frames; published_shadow_=result->shadow_generation;
                    }
                }
            }
            if(attempted_!=key && environment_has_frame_) {
                if(snapshot.scene.logical_bytes()>32U*1024U*1024U) throw std::runtime_error("Environment preview snapshot exceeds 32 MiB");
                environment_.request({snapshot,camera,width,height,edit,++environment_sequence_,exposure,environment});
                environment_key_=key; environment_camera_=camera; environment_has_frame_=false;
                attempted_=key; error_.clear();
            }
        } catch(const std::exception& error) { error_=error.what(); }
        return;
    }
    environment_.cancel(); environment_key_.reset(); environment_has_frame_=true;
    if (attempted_==key) return;
    attempted_=key;
    try {
        check(render::validate_environment_render_settings(environment));
        if (asset_session_!=key.session) {
            auto source=take(render::DiskScene::load_snapshot(heap_,snapshot.project,snapshot.scene,{GltfImportProfile::unlit_preview}));
            std::vector<PickMesh> queries;
            for (const auto& asset:source.assets()) {
                std::vector<geometry::Triangle> triangles;
                for (const auto& p:asset->mesh.primitives) for (std::size_t i=0;i+2<p.indices.size();i+=3)
                    triangles.push_back({p.positions[p.indices[i]].cast<double>(),p.positions[p.indices[i+1]].cast<double>(),p.positions[p.indices[i+2]].cast<double>()});
                queries.push_back({asset->mesh.id,take(geometry::MeshQuery::build(triangles))});
            }
            auto candidate=take(source.upload(queue_));
            assets_=std::move(candidate);
            meshes_=std::move(queries);
            asset_session_=key.session;
        }
        std::optional<render::LocalTransformOverride> transform;
        if (edit) transform=render::LocalTransformOverride{edit->entity,edit->value};
        auto scene=take(render::RenderScene::extract(heap_,snapshot.scene,transform));
        auto render_view=take(render::RenderView::create(scene,{width,height,info_.frame+1,camera.world(),camera.projection(double(width)/height)}));
        auto settings=render::unlit_preview_settings(); settings.exposure=exposure;
        auto frame=take(pipeline_.render(queue_,render_view,assets_,settings));
        if (!take(frame.wait(queue_))) throw std::runtime_error("Viewport render timed out");
        std::vector<std::byte> pixels(static_cast<std::size_t>(width)*height*4);
        check(frame.read_rgba8(pixels));
        publish(std::move(pixels),frame.info(),std::move(scene),camera,key);
        published_environment_=environment; published_history_={}; published_warmup_=0; published_shadow_=0;
    } catch (const std::exception& error) { error_=error.what(); }
}
void Viewport::publish(std::vector<std::byte> pixels,const render::FrameInfo& info,render::RenderScene scene,const Camera& camera,const Key& key) {
        // ScenePipeline returns encoded sRGB. An sRGB view decodes before the sRGB swapchain encodes.
        graphics::ImageDesc desc{key.width,key.height,key.srgb ? vk::Format::eR8G8B8A8Srgb : vk::Format::eR8G8B8A8Unorm};
        auto image=take(queue_.create_image(desc));
        auto view=take(queue_.resources().create_view(image));
        auto batch=take(queue_.begin());
        check(batch.upload(image,pixels,{0,0,0,0,key.width,key.height}));
        check(batch.transition(image,vk::ImageLayout::eShaderReadOnlyOptimal));
        auto submission=take(queue_.submit(std::move(batch)));
        if (!take(queue_.wait(submission))) throw std::runtime_error("Viewport upload timed out");
        auto descriptor=ImGui_ImplVulkan_AddTexture(static_cast<VkImageView>(view.handle()),VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        if (!descriptor) throw std::runtime_error("Viewport texture registration failed");
        release_texture();
        image_=std::move(image); view_=std::move(view); descriptor_=descriptor;
        pixel_signature_=14695981039346656037ULL;
        for (const auto byte : pixels) { pixel_signature_^=std::to_integer<unsigned char>(byte); pixel_signature_*=1099511628211ULL; }
        info_=info; published_session_=key.session; attempted_=key; published_=key; error_.clear();
        scene_=std::move(scene); camera_=camera; published_camera_=camera.revision();
        if (retain_pixels_) pixels_=std::move(pixels);
}

Result<std::optional<EntityId>> Viewport::pick(const Vec2d& uv) const {
    if (!scene_ || !error_.empty()) return std::unexpected(Error{ErrorCode::invalid_state,"Preview unavailable for picking"});
    auto ray=camera_.ray(uv,double(info_.width)/info_.height); if (!ray) return std::unexpected(ray.error());
    std::optional<EntityId> selected; double distance=ray->maximum;
    for (std::size_t i=0;i<scene_.entities().size();++i) {
        const auto& entity=scene_.entities()[i];
        if (!entity.world.inverse()) continue; // Zero-scale entities have no pickable volume.
        for (const auto& asset:take(scene_.assets(i))) if (asset.kind==AssetKind::mesh) {
            const auto mesh=std::ranges::find(meshes_,asset.id,&PickMesh::id);
            if (mesh==meshes_.end()) continue;
            auto hit=mesh->query.nearest(*ray,entity.world); if (!hit) return std::unexpected(hit.error());
            if (*hit && (!selected || (*hit)->distance<distance || ((*hit)->distance==distance && entity.id<*selected))) {
                selected=entity.id; distance=(*hit)->distance;
            }
        }
    }
    return selected;
}
geometry::Bounds Viewport::bounds(EntityId id,const SceneSnapshot& snapshot) const {
    geometry::Bounds result;
    for (std::size_t i=0;i<scene_.entities().size();++i) {
        const auto& entity=scene_.entities()[i];
        auto ancestor=std::optional{entity.id}; bool included=false;
        for (std::size_t depth=0;ancestor && depth<=snapshot.entities().size();++depth) {
            if (*ancestor==id) { included=true; break; }
            auto e=std::ranges::find(snapshot.entities(),*ancestor,&EntityData::id);
            ancestor=e==snapshot.entities().end() ? std::nullopt : e->parent;
        }
        if (!included) continue;
        for (const auto& asset:take(scene_.assets(i))) if (asset.kind==AssetKind::mesh) {
            const auto mesh=std::ranges::find(meshes_,asset.id,&PickMesh::id);
            if (mesh==meshes_.end() || mesh->query.bounds().empty()) continue;
            const auto& b=mesh->query.bounds();
            for (int corner=0;corner<8;++corner) {
                const Vec3d p{corner&1 ? b.maximum.x():b.minimum.x(),corner&2 ? b.maximum.y():b.minimum.y(),corner&4 ? b.maximum.z():b.minimum.z()};
                auto world=entity.world.transform_point(p); if (world) result.include(*world);
            }
        }
    }
    if (result.empty()) if (auto world=entity_world(snapshot,id)) {
        const Vec3d p=world->matrix().block<3,1>(0,3);
        result.include(p-Vec3d::Constant(0.1)); result.include(p+Vec3d::Constant(0.1));
    }
    return result;
}
}
