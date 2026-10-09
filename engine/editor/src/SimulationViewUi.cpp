#include "WorkbenchUi.hpp"
#include <algorithm>

namespace dk::editor::detail {
void WorkbenchUi::frame_simulation(const ClothConfig& cloth,double aspect) {
    geometry::Bounds bounds;
    const double half=(cloth.columns-1)*cloth.spacing/2.0,depth=(cloth.rows-1)*cloth.spacing;
    bounds.include({-half,std::min(double(cloth.physics.floor_y),double(cloth.height)),0});
    bounds.include({half,cloth.height,depth});
    simulation_camera_.reset(true);
    simulation_camera_.orbit(65,70);
    simulation_camera_.frame(bounds,aspect);
}
void WorkbenchUi::simulation_navigation() {
    auto& io=ImGui::GetIO();
    mark("simulation_viewport");
    const bool modal=ImGui::IsPopupOpen(nullptr,ImGuiPopupFlags_AnyPopupId|ImGuiPopupFlags_AnyPopupLevel);
    if (io.AppFocusLost || modal || io.WantTextInput || ImGui::IsKeyPressed(ImGuiKey_Escape,false)) { simulation_navigation_=-1; return; }
    const auto size=ImGui::GetItemRectSize();
    const bool hovered=ImGui::IsItemHovered();
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) simulation_navigation_=ImGuiMouseButton_Right;
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Middle)) simulation_navigation_=ImGuiMouseButton_Middle;
    if (simulation_navigation_>=0 && !ImGui::IsMouseDown(simulation_navigation_)) simulation_navigation_=-1;
    if (simulation_navigation_==ImGuiMouseButton_Right && !ImGui::IsMouseClicked(simulation_navigation_)) simulation_camera_.orbit(io.MouseDelta.x,io.MouseDelta.y);
    if (simulation_navigation_==ImGuiMouseButton_Middle && !ImGui::IsMouseClicked(simulation_navigation_)) simulation_camera_.pan(io.MouseDelta.x,io.MouseDelta.y,size.y);
    if (hovered && io.MouseWheel!=0) simulation_camera_.dolly(io.MouseWheel);
    if (hovered && simulation_cloth_ && (ImGui::IsKeyPressed(ImGuiKey_F,false) || ImGui::IsKeyPressed(ImGuiKey_Home,false)))
        frame_simulation(*simulation_cloth_,size.x/size.y);
}
bool WorkbenchUi::simulation_view(Viewport& preview,bool visible) {
    const auto state=model_.simulation_state();
    const bool active=state.run && state.run->cloth && state.mode!=SimulationMode::stopping;
    if (!active) { simulation_camera_run_.reset(); simulation_navigation_=-1; return false; }
    if (simulation_camera_run_!=state.run->run_id) {
        simulation_camera_run_=state.run->run_id; simulation_cloth_=state.run->cloth;
        simulation_visible_=true; input_.cancel();
        const auto available=ImGui::GetContentRegionAvail();
        frame_simulation(*simulation_cloth_,std::max(0.1f,available.x/std::max(32.0f,available.y-80)));
    }
    ImGui::BeginDisabled(input_.active());
    if (ImGui::RadioButton("Simulation view",simulation_visible_)) { simulation_visible_=true; input_.cancel(); }
    mark("simulation_view"); ImGui::SameLine();
    if (ImGui::RadioButton("Edit view",!simulation_visible_)) { simulation_visible_=false; simulation_navigation_=-1; }
    mark("simulation_edit_view"); ImGui::EndDisabled();
    if (!simulation_visible_) return false;
    input_.cancel();
    if (!visible) { simulation_navigation_=-1; return true; }
    if (ImGui::SmallButton("Frame cloth")) {
        const auto available=ImGui::GetContentRegionAvail();
        frame_simulation(*simulation_cloth_,available.x/std::max(32.0f,available.y-60));
    }
    mark("simulation_frame"); ImGui::SameLine();
    if (ImGui::SmallButton("Retry preview")) preview.retry_simulation();
    ImGui::TextDisabled("RMB: orbit | MMB: pan | wheel: zoom | F/Home: frame");
    if (preview.texture() && preview.simulation_run()==state.run->run_id)
        ImGui::Text("%s cloth | displayed step %llu | %s",state.run->gpu ? "GPU" : "CPU",
            static_cast<unsigned long long>(preview.simulation_steps()),preview.simulation_view_current() ? "Completed image" : "Updating view...");
    else ImGui::TextUnformatted("Preparing cloth preview...");
    if (!preview.error().empty()) ImGui::TextWrapped("Preview: %s",preview.error().c_str());
    const auto available=ImGui::GetContentRegionAvail();
    if (available.x>=32 && available.y>=32) {
        const auto width=static_cast<std::uint32_t>(std::clamp(available.x,32.0f,1600.0f));
        const auto height=static_cast<std::uint32_t>(std::clamp(available.y,32.0f,1200.0f));
        preview.update_simulation(model_,*state.run,width,height,simulation_camera_);
        if (preview.texture() && preview.simulation_run()==state.run->run_id) {
            const auto& info=preview.info();
            const float scale=std::min(available.x/info.width,available.y/info.height);
            ImGui::Image(preview.texture(),{info.width*scale,info.height*scale});
            simulation_navigation();
        }
    } else simulation_navigation_=-1;
    return true;
}
}
