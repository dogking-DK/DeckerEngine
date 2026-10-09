#include "Viewport.hpp"
#include "EditorSupport.hpp"
#include <dk/graphics/Transfer.hpp>
#include <imgui_impl_vulkan.h>
#ifdef DK_SIMULATION_GPU
#include <dk/services/SimulationPreview.hpp>
#endif

namespace dk::editor::detail {
void Viewport::show_edit() {
    if (!simulation_run_) return;
    release_texture(); simulation_run_.reset(); error_.clear();
    view_={}; image_={}; info_={}; pixels_.clear();
}
void Viewport::update_simulation(Workspace& model,const SimulationRunState& run,std::uint32_t width,std::uint32_t height,const Camera& camera) {
    if (simulation_run_!=run.run_id) {
        release_texture(); view_={}; image_={}; info_={}; pixels_.clear(); error_.clear();
        simulation_run_=run.run_id; simulation_steps_=0;
    }
#ifdef DK_SIMULATION_GPU
    if (run.fault) { error_="Simulation faulted; showing the last completed preview."; return; }
    try {
        SimulationView view{width,height};
        const auto matrix=camera.clip(double(width)/height).cast<float>().eval();
        for (int row=0;row<4;++row) for (int col=0;col<4;++col) view.view_projection[row*4+col]=matrix(row,col);
        view.sequence=simulation_view_ ? simulation_view_->sequence : 0;
        if (!simulation_view_ || *simulation_view_!=view) {
            view.sequence=++simulation_sequence_; simulation_view_=view;
        }
        check(model.request_simulation_preview(run.run_id,view));
        auto result=take(model.take_simulation_preview(run.run_id));
        if (!result || result->run_id!=run.run_id || result->view!=view) return;
        auto image=take(queue_.import_image(std::move(result->image)));
        auto image_view=take(queue_.resources().create_view(image));
        auto descriptor=ImGui_ImplVulkan_AddTexture(static_cast<VkImageView>(image_view.handle()),VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        if (!descriptor) throw std::runtime_error("Simulation texture registration failed");
        if (descriptor_) ImGui_ImplVulkan_RemoveTexture(descriptor_);
        image_=std::move(image); view_=std::move(image_view); descriptor_=descriptor;
        info_.width=width; info_.height=height; info_.draw_count=1; ++info_.frame;
        simulation_published_=view.sequence; simulation_steps_=result->steps;
        published_camera_=camera.revision(); camera_=camera; error_.clear();
    } catch (const std::exception& error) { error_=error.what(); }
#else
    error_="Simulation preview is not compiled in this editor.";
#endif
}
std::uint64_t Viewport::capture_simulation_pixels() {
    if (!simulation_run_ || !descriptor_) throw std::runtime_error("No simulation image to inspect");
    auto batch=take(queue_.begin());
    auto readback=take(batch.readback(image_,{0,0,0,0,info_.width,info_.height}));
    check(batch.transition(image_,vk::ImageLayout::eShaderReadOnlyOptimal));
    const auto submission=take(queue_.submit(std::move(batch)));
    if (!take(queue_.wait(submission))) throw std::runtime_error("Simulation image inspection timed out");
    pixels_.resize(static_cast<std::size_t>(info_.width)*info_.height*4);
    if (!take(readback.try_read(pixels_))) throw std::runtime_error("Simulation image inspection incomplete");
    pixel_signature_=14695981039346656037ULL;
    for (const auto b:pixels_) { pixel_signature_^=std::to_integer<unsigned char>(b); pixel_signature_*=1099511628211ULL; }
    return pixel_signature_;
}
}
