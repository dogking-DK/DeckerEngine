#include "WorkbenchUi.hpp"
#include <algorithm>

namespace dk::editor::detail {
void WorkbenchUi::frame_simulation(const ClothConfig& cloth,double aspect) {
    simulation_navigation_.cancel();
    geometry::Bounds bounds;
    const double half=(cloth.columns-1)*cloth.spacing/2.0,depth=(cloth.rows-1)*cloth.spacing;
    bounds.include({-half,std::min(double(cloth.physics.floor_y),double(cloth.height)),0});
    bounds.include({half,cloth.height,depth});
    Camera candidate=simulation_camera_;
    if(candidate.projection_mode()==ProjectionMode::perspective) candidate.reset(true);
    // Framing orientation is independent of the user's navigation sensitivity/invert preference.
    auto settings=candidate.settings(),orientation=settings;
    orientation.navigation.orbit_sensitivity=.006; orientation.navigation.invert_y=false;
    if(!report("Frame cloth",candidate.set_settings(orientation,aspect))) return;
    if(candidate.projection_mode()==ProjectionMode::perspective) candidate.orbit(65,70);
    if(!report("Frame cloth",candidate.set_settings(settings,aspect))) return;
    if(report("Frame cloth",candidate.frame(bounds,aspect))) simulation_camera_=candidate;
}
void WorkbenchUi::simulation_navigation() {
    mark("simulation_viewport");
    simulation_navigation_.image(simulation_camera_,false);
    auto& io=ImGui::GetIO();
    const bool popup=ImGui::IsPopupOpen(nullptr,ImGuiPopupFlags_AnyPopupId|ImGuiPopupFlags_AnyPopupLevel);
    if (!io.AppFocusLost && !io.WantTextInput && !popup && ImGui::IsItemHovered() && simulation_cloth_ &&
        (ImGui::IsKeyPressed(ImGuiKey_F,false) || ImGui::IsKeyPressed(ImGuiKey_Home,false))) {
        const auto size=ImGui::GetItemRectSize(); frame_simulation(*simulation_cloth_,size.x/size.y);
    }
}

bool WorkbenchUi::simulation_view(Viewport& preview,bool visible) {
    const auto state=model_.simulation_state();
    const bool active=state.run && state.run->cloth && state.mode!=SimulationMode::stopping;
    if (!active) { simulation_camera_run_.reset(); simulation_navigation_.cancel(); return false; }
    if (simulation_camera_run_!=state.run->run_id) {
        simulation_camera_run_=state.run->run_id; simulation_cloth_=state.run->cloth;
        simulation_visible_=true; input_.cancel();
        const auto available=ImGui::GetContentRegionAvail();
        frame_simulation(*simulation_cloth_,std::max(0.1f,available.x/std::max(32.0f,available.y-80)));
    }
    ImGui::BeginDisabled(input_.active());
    if (ImGui::RadioButton("Simulation view",simulation_visible_)) { simulation_visible_=true; input_.cancel(); }
    mark("simulation_view"); ImGui::SameLine();
    if (ImGui::RadioButton("Edit view",!simulation_visible_)) { simulation_visible_=false; simulation_navigation_.cancel(); }
    mark("simulation_edit_view"); ImGui::EndDisabled();
    if (!simulation_visible_) return false;
    input_.cancel();
    if (!visible) { simulation_navigation_.cancel(); return true; }
    if (ImGui::SmallButton("Frame cloth")) {
        const auto available=ImGui::GetContentRegionAvail();
        frame_simulation(*simulation_cloth_,available.x/std::max(32.0f,available.y-60));
    }
    mark("simulation_frame"); ImGui::SameLine();
    if (ImGui::SmallButton("Retry preview")) preview.retry_simulation();
    ImGui::SameLine();
    const auto camera_revision=simulation_camera_.revision();
    if(simulation_settings_ui_.draw(simulation_camera_,true,simulation_aspect_,controls_,"simulation_camera_") || simulation_camera_.revision()!=camera_revision) simulation_navigation_.cancel();
    simulation_navigation_.toolbar(simulation_camera_,controls_,"simulation_camera_");
    if (preview.texture() && preview.simulation_run()==state.run->run_id)
        ImGui::Text("%s cloth | displayed step %llu | %s",state.run->gpu ? "GPU" : "CPU",
            static_cast<unsigned long long>(preview.simulation_steps()),preview.simulation_view_current() ? "Completed image" : "Updating view...");
    else ImGui::TextUnformatted("Preparing cloth preview...");
    if (!preview.error().empty()) ImGui::TextWrapped("Preview: %s",preview.error().c_str());
    const auto available=ImGui::GetContentRegionAvail();
    if (available.x>=32 && available.y>=32) {
        const auto width=static_cast<std::uint32_t>(std::clamp(available.x,32.0f,1600.0f));
        const auto height=static_cast<std::uint32_t>(std::clamp(available.y,32.0f,1200.0f));
        simulation_aspect_=double(width)/height;
        preview.update_simulation(model_,*state.run,width,height,simulation_camera_);
        if (preview.texture() && preview.simulation_run()==state.run->run_id) {
            const auto& info=preview.info();
            const float scale=std::min(available.x/info.width,available.y/info.height);
            ImGui::Image(preview.texture(),{info.width*scale,info.height*scale});
            simulation_navigation();
        }
    } else simulation_navigation_.cancel();
    return true;
}
}
