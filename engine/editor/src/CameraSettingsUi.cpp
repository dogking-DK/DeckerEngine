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
        ImGui::Text("%s / %s",camera.navigation_mode()==NavigationMode::orbit ? "Orbit" : camera.navigation_mode()==NavigationMode::fly ? "Fly" : "Walk",
            camera.projection_mode()==ProjectionMode::orthographic ? "Orthographic" : "Perspective");
        ImGui::TextDisabled("Session settings for this viewport");
        ImGui::BeginDisabled(camera.projection_mode()==ProjectionMode::orthographic);
        ImGui::SetNextItemWidth(255);
        constexpr double minimum=10,maximum=120;
        ImGui::SliderScalar("FOV slider",ImGuiDataType_Double,&draft_.lens.fov_y,&minimum,&maximum,"%.1f deg");
        ImGui::InputDouble("Vertical FOV (deg)",&draft_.lens.fov_y,0,0,"%.3f"); mark("fov");
        ImGui::EndDisabled();
        ImGui::BeginDisabled(camera.projection_mode()!=ProjectionMode::orthographic);
        ImGui::InputDouble("Orthographic height (m)",&draft_.lens.ortho_height,0,0,"%.4f"); mark("ortho_height");
        ImGui::EndDisabled();
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
            if(result) { error_.clear(); ImGui::CloseCurrentPopup(); } else error_=result.error().message;
        }
        mark("apply"); ImGui::SameLine();
        if(ImGui::Button("Cancel")) ImGui::CloseCurrentPopup(); mark("cancel"); ImGui::SameLine();
        if(ImGui::Button("Restore defaults")) {
            const auto result=camera.restore_defaults(fixture,aspect);
            if(result) { error_.clear(); ImGui::CloseCurrentPopup(); } else error_=result.error().message;
        }
        mark("defaults"); ImGui::EndPopup();
    }
    const auto button=[](const char* label,bool selected) {
        if(selected) ImGui::PushStyleColor(ImGuiCol_Button,ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        const bool clicked=ImGui::SmallButton(label);
        if(selected) ImGui::PopStyleColor();
        return clicked;
    };
    const auto next=[](const char* label) {
        const float right=ImGui::GetWindowPos().x+ImGui::GetWindowContentRegionMax().x;
        if(ImGui::GetItemRectMax().x+ImGui::GetStyle().ItemSpacing.x+ImGui::CalcTextSize(label).x+2*ImGui::GetStyle().FramePadding.x<right)
            ImGui::SameLine();
    };
    if(button("Perspective",camera.projection_mode()==ProjectionMode::perspective)) {
        const auto result=camera.set_projection_mode(ProjectionMode::perspective,aspect);
        error_=result ? "" : result.error().message;
    }
    mark("perspective"); next("Orthographic");
    ImGui::BeginDisabled(!camera.orthographic_supported());
    if(button("Orthographic",camera.projection_mode()==ProjectionMode::orthographic)) {
        const auto result=camera.set_projection_mode(ProjectionMode::orthographic,aspect);
        error_=result ? "" : result.error().message;
    }
    mark("orthographic"); ImGui::EndDisabled();
    constexpr const char* names[]={"Front","Back","Left","Right","Top","Bottom"};
    constexpr const char* keys[]={"front","back","left","right","top","bottom"};
    ImGui::BeginDisabled(camera.projection_mode()!=ProjectionMode::orthographic || !camera.orthographic_supported());
    for(int i=0;i<6;++i) {
        next(names[i]);
        const auto view=static_cast<StandardView>(i+1);
        if(button(names[i],camera.standard_view()==view)) {
            const auto result=camera.set_standard_view(view,aspect); error_=result ? "" : result.error().message;
        }
        mark(keys[i]);
    }
    ImGui::EndDisabled();
    if(!error_.empty() && !open) ImGui::TextWrapped("%s",error_.c_str());
    ImGui::PopID(); return open;
}
}
