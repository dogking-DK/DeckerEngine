#include "WorkbenchUi.hpp"
#include <algorithm>
namespace dk::editor::detail {
void WorkbenchUi::path_panel(Viewport& preview) {
    ImGui::Begin("Path tracing");
    if(!preview.path_unavailable().empty()) { ImGui::TextWrapped("%s",preview.path_unavailable().c_str()); ImGui::End(); return; }
    ImGui::TextUnformatted(path_mode_ ? "GPU path tracing" : "Select Path trace in Viewport");
    if(ImGui::RadioButton("Surface only",!path_settings_.scene_media)) path_settings_.scene_media=false; mark("path_surface");
    ImGui::SameLine(); if(ImGui::RadioButton("Scene media",path_settings_.scene_media)) path_settings_.scene_media=true; mark("path_media");
    ImGui::TextWrapped(path_settings_.scene_media ? "Trace scene air, clouds and surfaces at a fixed time." : "Trace surface lighting; omit scene air and clouds.");
    int target=int(path_settings_.target),seed=int(path_settings_.seed),bounces=int(path_settings_.bounces);
    ImGui::SetNextItemWidth(125);
    if(ImGui::InputInt("Target spp",&target)) path_settings_.target=std::uint32_t(std::clamp(target,1,65536)); mark("path_target"); controls_["path_target"].x=ImGui::GetItemRectMin().x+30;
    ImGui::SetNextItemWidth(125);
    if(ImGui::InputInt("Seed",&seed)) path_settings_.seed=std::uint32_t(std::max(0,seed)); mark("path_seed"); controls_["path_seed"].x=ImGui::GetItemRectMin().x+30;
    ImGui::SetNextItemWidth(125);
    if(ImGui::InputInt("Bounces",&bounces)) path_settings_.bounces=std::uint32_t(std::clamp(bounces,1,16)); mark("path_bounces"); controls_["path_bounces"].x=ImGui::GetItemRectMin().x+30;
    float sky[3]={float(path_settings_.sky.x()),float(path_settings_.sky.y()),float(path_settings_.sky.z())};
    ImGui::SetNextItemWidth(160);
    if(ImGui::ColorEdit3("Environment",sky,ImGuiColorEditFlags_Float|ImGuiColorEditFlags_HDR))
        path_settings_.sky=Vec3d{sky[0],sky[1],sky[2]}.cwiseMax(0).cwiseMin(1000);
    ImGui::Checkbox("Scene sun",&path_settings_.sun); mark("path_sun");
    float exposure=float(path_settings_.exposure); ImGui::SetNextItemWidth(125);
    if(ImGui::InputFloat("Exposure (EV)",&exposure,.25f,1,"%.2f")) path_settings_.exposure=std::clamp(double(exposure),-16.,16.); mark("path_exposure"); controls_["path_exposure"].x=ImGui::GetItemRectMin().x+30;
    if(ImGui::Button(path_settings_.paused ? "Resume" : "Pause")) path_settings_.paused=!path_settings_.paused; mark("path_pause");
    ImGui::SameLine(); if(ImGui::Button("Reset accumulation")) ++path_settings_.reset; mark("path_reset");
    if(path_settings_.scene_media) {
        ImGui::SetNextItemWidth(125);
        if(ImGui::InputDouble("Time (s)",&path_settings_.time_seconds,1,10,"%.3f")) path_settings_.time_seconds=std::clamp(path_settings_.time_seconds,-1e9,1e9); mark("path_time");
        controls_["path_time"].x=ImGui::GetItemRectMin().x+30;
        ImGui::SetNextItemWidth(125);
        if(ImGui::InputDouble("Cloud albedo",&path_settings_.cloud_albedo,.05,.1,"%.3f")) path_settings_.cloud_albedo=std::clamp(path_settings_.cloud_albedo,0.,1.);
        ImGui::SetNextItemWidth(125);
        if(ImGui::InputDouble("Cloud anisotropy",&path_settings_.cloud_anisotropy,.05,.1,"%.3f")) path_settings_.cloud_anisotropy=std::clamp(path_settings_.cloud_anisotropy,-.95,.95);
    }
    if(preview.path_samples()) {
        const auto& used=preview.path_info();
        ImGui::TextWrapped("Shown: %s / model %u / sampler %u",used.medium ? "Scene media" : "Surface only",
            used.medium ? used.medium->settings.model_version : used.settings.model_version,
            used.medium ? used.medium->settings.sampler_version : used.settings.sampler_version);
        if(used.medium) ImGui::Text("t %.3f / field %016llx",used.medium->medium.time_seconds,static_cast<unsigned long long>(used.medium->field_hash));
    }
    const auto status=preview.path_status(); const auto& p=status.progress;
    ImGui::Text("%s: %u / %u spp",status.phase.c_str(),p.completed_spp,p.target_spp);
    ImGui::ProgressBar(p.target_spp ? float(p.completed_spp)/float(p.target_spp) : 0,{-1,0});
    ImGui::Text("Tiles %u / %u  |  shown %u spp",p.completed_tiles,p.total_tiles,preview.path_samples());
    if(!status.error.empty()) ImGui::TextWrapped("%s",status.error.c_str());
    ImGui::TextWrapped("Preview imports base color textures and material factors. Normal, occlusion and metallic-roughness textures and tangents are omitted.");
    ImGui::TextWrapped("Limits: 1 million triangles, 64 textures / 128 MiB source pixels, 1 GiB GPU budget. Exposure preserves accumulated samples.");
    ImGui::End();
}
}
