#include "Viewport.hpp"
#include "EditorSupport.hpp"
#include <imgui_impl_vulkan.h>
#include <cmath>
#include <algorithm>
#include <vector>

namespace dk::editor::detail {
Viewport::Viewport(memory::ResourceHandle heap,graphics::SubmissionQueue& queue,bool retain_pixels)
    : heap_(heap),queue_(queue),pipeline_(take(render::ScenePipeline::create(heap,queue,DK_EDITOR_SHADER_DIR))),retain_pixels_(retain_pixels) {}
Viewport::~Viewport() { release_texture(); }
void Viewport::release_texture() {
    if (descriptor_) ImGui_ImplVulkan_RemoveTexture(descriptor_);
    descriptor_=VK_NULL_HANDLE;
    attempted_.reset();
}
void Viewport::update(const SceneReadSnapshot& snapshot,std::uint32_t width,std::uint32_t height,bool srgb,const Camera& camera,
    const std::optional<TransformEdit>& edit,std::uint64_t preview_revision) {
    const Key key{snapshot.state.document_id,snapshot.state.revision,width,height,srgb,camera.revision(),preview_revision};
    if (attempted_==key) return;
    attempted_=key;
    try {
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
        auto frame=take(pipeline_.render(queue_,render_view,assets_,render::unlit_preview_settings()));
        if (!take(frame.wait(queue_))) throw std::runtime_error("Viewport render timed out");
        std::vector<std::byte> pixels(static_cast<std::size_t>(width)*height*4);
        check(frame.read_rgba8(pixels));
        // ScenePipeline returns encoded sRGB. An sRGB view decodes before the sRGB swapchain encodes.
        graphics::ImageDesc desc{width,height,srgb ? vk::Format::eR8G8B8A8Srgb : vk::Format::eR8G8B8A8Unorm};
        auto image=take(queue_.create_image(desc));
        auto view=take(queue_.resources().create_view(image));
        auto batch=take(queue_.begin());
        check(batch.upload(image,pixels,{0,0,0,0,width,height}));
        check(batch.transition(image,vk::ImageLayout::eShaderReadOnlyOptimal));
        auto submission=take(queue_.submit(std::move(batch)));
        if (!take(queue_.wait(submission))) throw std::runtime_error("Viewport upload timed out");
        auto descriptor=ImGui_ImplVulkan_AddTexture(static_cast<VkImageView>(view.handle()),VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        if (!descriptor) throw std::runtime_error("Viewport texture registration failed");
        release_texture();
        image_=std::move(image); view_=std::move(view); descriptor_=descriptor;
        pixel_signature_=14695981039346656037ULL;
        for (const auto byte : pixels) { pixel_signature_^=std::to_integer<unsigned char>(byte); pixel_signature_*=1099511628211ULL; }
        info_=frame.info(); published_session_=key.session; attempted_=key; error_.clear();
        scene_=std::move(scene); camera_=camera; published_camera_=camera.revision();
        if (retain_pixels_) pixels_=std::move(pixels);
    } catch (const std::exception& error) { error_=error.what(); }
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
