#include "CameraSettingsUi.hpp"

namespace dk::editor::detail {
bool CameraSettingsUi::draw(Camera& camera,bool fixture,double aspect,std::map<std::string,ImVec2>& controls,const char* prefix) {
    ImGui::PushID(prefix);
    const auto mark=[&](const char* name) {
        const auto a=ImGui::GetItemRectMin(),b=ImGui::GetItemRectMax();
        controls[std::string(prefix)+name]={(a.x+b.x)/2,(a.y+b.y)/2};
    };
    if(ImGui::SmallButton("Camera")) { draft_=camera.settings(); error_.clear(); ImGui::OpenPopup("Camera settings"); }
    mark("settings");
    ImGui::SetNextWindowSize({460,0},ImGuiCond_Appearing);
    const bool open=ImGui::BeginPopup("Camera settings");
    if(open) {
        ImGui::TextUnformatted("Orbit / Perspective");
        ImGui::TextDisabled("Session settings for this viewport");
        ImGui::SetNextItemWidth(255);
        constexpr double minimum=10,maximum=120;
        ImGui::SliderScalar("FOV slider",ImGuiDataType_Double,&draft_.lens.fov_y,&minimum,&maximum,"%.1f deg");
        ImGui::InputDouble("Vertical FOV (deg)",&draft_.lens.fov_y,0,0,"%.3f"); mark("fov");
        ImGui::Checkbox("Auto clipping",&draft_.lens.automatic_clip); mark("auto_clip");
        ImGui::BeginDisabled(draft_.lens.automatic_clip);
        ImGui::InputDouble("Near (m)",&draft_.lens.near_plane,0,0,"%.4f"); mark("near");
        ImGui::InputDouble("Far (m)",&draft_.lens.far_plane,0,0,"%.2f"); mark("far");
        ImGui::EndDisabled();
        ImGui::TextDisabled("Applied near %.4g / far %.4g m",camera.near_plane(),camera.far_plane());
        ImGui::Separator();
        ImGui::InputDouble("Orbit (rad/pixel)",&draft_.navigation.orbit_sensitivity,0,0,"%.4f");
        ImGui::InputDouble("Pan multiplier",&draft_.navigation.pan_multiplier,0,0,"%.3f");
        ImGui::InputDouble("Wheel sensitivity",&draft_.navigation.wheel_sensitivity,0,0,"%.3f");
        ImGui::Checkbox("Invert mouse Y",&draft_.navigation.invert_y);
        ImGui::InputDouble("Move speed (m/s)",&draft_.navigation.move_speed,0,0,"%.3f"); mark("speed");
        ImGui::TextDisabled("Saved for future Fly/Walk navigation.");
        if(ImGui::TreeNode("Fly/Walk preferences")) {
            ImGui::InputDouble("Fast multiplier",&draft_.navigation.fast_multiplier,0,0,"%.3f");
            ImGui::InputDouble("Slow multiplier",&draft_.navigation.slow_multiplier,0,0,"%.3f");
            ImGui::InputDouble("Look (rad/pixel)",&draft_.navigation.look_sensitivity,0,0,"%.4f");
            ImGui::TreePop();
        }
        if(!error_.empty()) ImGui::TextWrapped("%s",error_.c_str());
        if(const auto valid=camera.validate(aspect); !valid) ImGui::TextWrapped("Current view: %s",valid.error().message.c_str());
        if(ImGui::Button("Apply")) {
            const auto result=camera.set_settings(draft_,aspect);
            if(result) ImGui::CloseCurrentPopup(); else error_=result.error().message;
        }
        mark("apply"); ImGui::SameLine();
        if(ImGui::Button("Cancel")) ImGui::CloseCurrentPopup(); mark("cancel"); ImGui::SameLine();
        if(ImGui::Button("Restore defaults")) {
            const auto result=camera.restore_defaults(fixture,aspect);
            if(result) ImGui::CloseCurrentPopup(); else error_=result.error().message;
        }
        mark("defaults"); ImGui::EndPopup();
    }
    ImGui::PopID(); return open;
}
}
