#include "WorkbenchUi.hpp"
#include <cmath>
#include <algorithm>
namespace dk::editor::detail {
void WorkbenchUi::environment(const Viewport& preview) {
    ImGui::Begin("Environment");
    auto* draft=model_.environment_draft();
    if(!draft) { ImGui::TextUnformatted("Open a scene to edit the environment."); ImGui::End(); return; }
    ImGui::TextWrapped("Meters, linear RGB. Apply creates one undo step. Unapplied values do not change the viewport.");
    ImGui::BeginDisabled(input_.active() || (model_.draft() && model_.draft()->modified));
    ImGui::TextUnformatted("Preset v1 (Apply to commit)");
    for(const char* name:{"clear","sparse","overcast","sunset"}) {
        if(name!=std::string_view{"clear"}) ImGui::SameLine();
        if(ImGui::Button(name)) report("Environment preset",model_.preset_environment(name));
        const auto id=std::string{"cloud_preset_"}+name; mark(id.c_str());
    }
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
    if(ImGui::CollapsingHeader("Cloud physical parameters")) {
        auto& c=e["clouds"]; bool clouds=c["enabled"].get<bool>();
        if(ImGui::Checkbox("Enable clouds",&clouds)) { c["enabled"]=clouds; draft->modified=true; } mark("cloud_enabled");
        for(const char* name:{"seed","weather_resolution","base_resolution","detail_resolution"}) {
            auto value=c[name].get<std::uint32_t>();
            if(ImGui::InputScalar(name,ImGuiDataType_U32,&value)) { c[name]=value; draft->modified=true; }
        }
        ImGui::Text("Generator version: %u",c["generator_version"].get<std::uint32_t>());
        for(const char* name:{"bottom_height_m","top_height_m","coverage","density","extinction_per_m","weather_scale_m","shape_scale_m","detail_scale_m","bottom_fade_fraction","top_fade_fraction"}) scalar(name,c[name]);
        double wind[]{c["wind_velocity_mps"][0].get<double>(),c["wind_velocity_mps"][1].get<double>()};
        if(ImGui::InputScalarN("Wind X/Z (m/s)",ImGuiDataType_Double,wind,2)) { c["wind_velocity_mps"]={wind[0],wind[1]}; draft->modified=true; }
    }
    ImGui::BeginDisabled(!draft->modified);
    if(ImGui::Button("Apply environment")) report("Environment Apply",model_.apply_environment()); mark("environment_apply");
    ImGui::SameLine(); if(ImGui::Button("Revert")) report("Environment Revert",model_.revert_environment()); mark("environment_revert");
    ImGui::EndDisabled(); ImGui::EndDisabled();
    ImGui::Separator(); ImGui::TextUnformatted("VIEWPORT");
    ImGui::InputFloat("Exposure",&exposure_,.1f,1,"%.3f");
    if(!std::isfinite(exposure_) || exposure_<0) exposure_=1;
    exposure_=std::min(exposure_,1000000.f);
    if(ImGui::CollapsingHeader("Cloud render quality and time")) {
        const bool reference_available=std::uint64_t{preview.info().width}*preview.info().height<=4096;
        if(ImGui::BeginCombo("Mode",environment_settings_.mode==render::CloudRenderMode::temporal ? "Temporal (progressive)" : "Reference (small viewport)")) {
            if(ImGui::Selectable("Temporal (progressive)",environment_settings_.mode==render::CloudRenderMode::temporal)) environment_settings_.mode=render::CloudRenderMode::temporal;
            ImGui::BeginDisabled(!reference_available);
            if(ImGui::Selectable("Reference (up to 4096 pixels)",environment_settings_.mode==render::CloudRenderMode::reference)) environment_settings_.mode=render::CloudRenderMode::reference;
            ImGui::EndDisabled(); ImGui::EndCombo();
        }
        ImGui::InputDouble("Time (s)",&environment_settings_.time_seconds,0,0,"%.6f"); mark("cloud_time");
        if(ImGui::Button("+1 second")) environment_settings_.time_seconds+=1; mark("cloud_time_step");
        ImGui::Checkbox("Cloud shadows",&environment_settings_.cloud_shadows); mark("cloud_shadow");
        ImGui::InputScalarN("Shadow center (m)",ImGuiDataType_Double,environment_settings_.shadow_center_m.data(),3);
        ImGui::InputDouble("Shadow extent (m)",&environment_settings_.shadow_extent_m);
        ImGui::InputScalar("Shadow resolution",ImGuiDataType_U32,&environment_settings_.shadow_resolution);
        ImGui::TextWrapped("Temporal shows a complete first frame, then refines while stationary. Reference requires a viewport of at most 4096 pixels. Time stays fixed until edited.");
    }
    ImGui::TextWrapped("Environment: near 1 m / far 10000 m. Disabled: near 0.05 m.");
    ImGui::End();
}
}
