#include "WorkbenchUi.hpp"
#include <cmath>
#include <algorithm>
namespace dk::editor::detail {
void WorkbenchUi::environment() {
    ImGui::Begin("Environment");
    auto* draft=model_.environment_draft();
    if(!draft) { ImGui::TextUnformatted("Open a scene to edit the environment."); ImGui::End(); return; }
    ImGui::TextWrapped("Meters, linear RGB. Apply creates one undo step. Unapplied values do not change the viewport.");
    ImGui::BeginDisabled(input_.active() || (model_.draft() && model_.draft()->modified));
    auto& e=draft->value; bool enabled=e.at("enabled").get<bool>();
    if(ImGui::Checkbox("Enable atmosphere",&enabled)) { e["enabled"]=enabled; draft->modified=true; } mark("environment_enabled");
    auto scalar=[&](const char* label,Json& value,const char* format="%.6g") {
        ImGui::TextWrapped("%s",label); ImGui::SetNextItemWidth(-1);
        const auto id=std::string{"##"}+label;
        double v=value.get<double>(); if(ImGui::InputDouble(id.c_str(),&v,0,0,format)) { value=v; draft->modified=true; }
    };
    auto vector=[&](const char* label,Json& value) {
        ImGui::TextWrapped("%s",label); ImGui::SetNextItemWidth(-1);
        const auto id=std::string{"##"}+label;
        double v[]{value[0].get<double>(),value[1].get<double>(),value[2].get<double>()};
        if(ImGui::InputScalarN(id.c_str(),ImGuiDataType_Double,v,3,nullptr,nullptr,"%.6g")) { value=Json::array({v[0],v[1],v[2]}); draft->modified=true; }
    };
    if(ImGui::CollapsingHeader("Sun",ImGuiTreeNodeFlags_DefaultOpen)) {
        vector("Direction",e["sun"]["direction"]); mark("environment_direction");
        vector("Irradiance",e["sun"]["irradiance"]);
        scalar("Angular radius (rad)",e["sun"]["angular_radius_rad"]);
    }
    if(ImGui::CollapsingHeader("Atmosphere parameters")) {
        auto& a=e["atmosphere"];
        for(const char* key:{"planet_center_m","rayleigh_scattering_per_m","mie_scattering_per_m","mie_extinction_per_m","absorption_extinction_per_m","ground_albedo"}) vector(key,a[key]);
        for(const char* key:{"ground_radius_m","atmosphere_height_m","rayleigh_scale_height_m","mie_scale_height_m","mie_anisotropy","absorption_center_height_m","absorption_half_width_m"}) scalar(key,a[key]);
    }
    ImGui::BeginDisabled(!draft->modified);
    if(ImGui::Button("Apply environment")) report("Environment Apply",model_.apply_environment()); mark("environment_apply");
    ImGui::SameLine(); if(ImGui::Button("Revert")) report("Environment Revert",model_.revert_environment()); mark("environment_revert");
    ImGui::EndDisabled(); ImGui::EndDisabled();
    ImGui::Separator(); ImGui::TextUnformatted("VIEWPORT");
    ImGui::InputFloat("Exposure",&exposure_,.1f,1,"%.3f");
    if(!std::isfinite(exposure_) || exposure_<0) exposure_=1;
    exposure_=std::min(exposure_,1000000.f);
    ImGui::TextWrapped("Environment: near 1 m / far 10000 m. Disabled: near 0.05 m. Static time 0; fixed atmosphere_v1 quality.");
    ImGui::End();
}
}
