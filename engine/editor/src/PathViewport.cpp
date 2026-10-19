#include "Viewport.hpp"
#include "EditorSupport.hpp"
#include <imgui_impl_vulkan.h>
namespace dk::editor::detail {
void Viewport::update_path(const SceneReadSnapshot& snapshot,std::uint32_t width,std::uint32_t height,bool srgb,const Camera& camera,
    const std::optional<TransformEdit>& edit,std::uint64_t preview,const PathPreviewSettings& settings) {
    show_edit(); environment_.cancel(); environment_key_.reset(); environment_has_frame_=true;
    if(!path_) { error_=path_unavailable_; return; }
    if(auto valid=camera.validate(double(width)/height); !valid) {
        suspend_path(); published_.reset(); error_=valid.error().message; return;
    }
    const auto now=std::chrono::steady_clock::now();
    if(!path_active_ || camera.revision()!=path_observed_camera_) {
        path_motion_until_=now+std::chrono::milliseconds(200); path_observed_camera_=camera.revision();
    }
    path_interactive_=settings.scene_media && now<path_motion_until_ && std::max(width,height)>192;
    if(path_interactive_) {
        const double scale=192.0/std::max(width,height);
        width=std::max(1u,std::uint32_t(width*scale)); height=std::max(1u,std::uint32_t(height*scale));
    }
    const Key key{snapshot.state.document_id,snapshot.state.revision,width,height,srgb,camera.revision(),preview,float(settings.exposure),{}};
    try {
        auto compatible=key;
        if(attempted_) { compatible.camera=attempted_->camera; compatible.width=attempted_->width; compatible.height=attempted_->height; }
        if(auto result=path_->take_result(); result && result->sequence==path_sequence_ && attempted_==compatible && path_settings_==settings) {
            auto image=take(queue_.import_image(std::move(result->image)));
            auto view=take(queue_.resources().create_view(image));
            auto descriptor=ImGui_ImplVulkan_AddTexture(static_cast<VkImageView>(view.handle()),VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            if(!descriptor) throw std::runtime_error("Path trace texture registration failed");
            if(descriptor_) ImGui_ImplVulkan_RemoveTexture(descriptor_);
            image_=std::move(image); view_=std::move(view); descriptor_=descriptor;
            info_={result->info.scene,result->info.revision,result->sequence,result->info.width,result->info.height,0};
            published_=attempted_; published_session_=result->document.document_id; published_camera_=result->camera.revision();
            camera_=result->camera; scene_=std::move(result->scene); meshes_=std::move(result->meshes);
            path_info_=result->info;
            path_samples_=result->info.samples.count; error_.clear();
            path_published_request_=path_requested_at_;
            path_linear_=std::move(result->linear);
        }
        auto same_camera=key; if(attempted_) same_camera.camera=attempted_->camera;
        const bool camera_only=path_active_ && path_settings_==settings && attempted_==same_camera;
        const auto worker=path_->status();
        const bool preparing=worker.phase=="Queued" || worker.phase=="Compiling" || worker.phase=="Loading assets" || worker.phase=="Building scene";
        // Let a short preview finish during a continuous gesture. Physical edits,
        // resize, refresh and idle restoration always invalidate immediately.
        const bool ready=!camera_only || settings.paused || published_==attempted_ || worker.phase=="Failed" ||
            (settings.scene_media && !preparing && now-path_requested_at_>=std::chrono::milliseconds(100));
        if(ready && (!path_active_ || attempted_!=key || path_settings_!=settings)) {
            if(snapshot.scene.logical_bytes()>32U*1024U*1024U) throw std::runtime_error("Path preview snapshot exceeds 32 MiB");
            if(!path_active_) path_samples_=0;
            path_->request({snapshot,camera,width,height,srgb,edit,preview,path_reload_,++path_sequence_,settings,retain_pixels_});
            attempted_=key; published_.reset(); path_settings_=settings; path_active_=true; error_.clear(); path_requested_at_=now;
        }
        const auto status=path_->status(); if(status.sequence==path_sequence_ && !status.error.empty()) error_=status.error;
    } catch(const std::exception& error) { error_=error.what(); }
}
std::uint64_t Viewport::capture_path_pixels() {
    if(!path_active_ || !descriptor_) throw std::runtime_error("No path image to inspect");
    auto batch=take(queue_.begin());
    auto readback=take(batch.readback(image_,{0,0,0,0,info_.width,info_.height}));
    check(batch.transition(image_,vk::ImageLayout::eShaderReadOnlyOptimal));
    if(!take(queue_.wait(take(queue_.submit(std::move(batch)))))) throw std::runtime_error("Path image inspection timed out");
    pixels_.resize(static_cast<std::size_t>(info_.width)*info_.height*4);
    if(!take(readback.try_read(pixels_))) throw std::runtime_error("Path image inspection incomplete");
    pixel_signature_=14695981039346656037ULL;
    for(const auto b:pixels_) { pixel_signature_^=std::to_integer<unsigned char>(b); pixel_signature_*=1099511628211ULL; }
    return pixel_signature_;
}
}
