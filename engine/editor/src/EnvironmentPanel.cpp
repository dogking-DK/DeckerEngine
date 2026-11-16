#include "WorkbenchUi.hpp"
#include "SunDirection.hpp"
#include <imgui_internal.h>
#include <cmath>
#include <algorithm>
namespace dk::editor::detail {
void WorkbenchUi::environment(const Viewport& preview) {
    settings_help_.panel("Environment"); ImGui::Begin("Environment");
    auto* draft=model_.environment_draft();
    if(!draft) { environment_error_.clear(); ImGui::TextUnformatted("Open a scene to edit the environment."); settings_help_.unavailable_panel("Environment"); ImGui::End(); return; }
    if(!draft->modified) environment_error_.clear();
    const bool blocked=input_.active() || input_.navigating() || (model_.draft() && model_.draft()->modified);
    if(blocked) ImGui::TextWrapped("Finish the viewport gesture or Apply/Revert the Inspector edits first.");
    ImGui::TextWrapped("Changes apply automatically. Release a drag or finish typing to update. Undo restores each edit.");
    if(!environment_error_.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text,ImVec4{.9f,.3f,.15f,1});
        ImGui::TextWrapped("Not applied: %s",environment_error_.c_str()); ImGui::PopStyleColor();
    } else ImGui::TextDisabled(draft->modified ? "Editing - viewport shows the last applied values" : "All changes applied to the scene");
    const auto& settings=model_.editor_viewport()->settings();
    const auto& applied=model_.snapshot()->scene.environment();
    if(applied.enabled || applied.clouds.enabled) {
        bool needs_settings=false;
        if(settings.path_trace && !settings.path.scene_media) { needs_settings=true; ImGui::TextWrapped("Surface only omits air and clouds. Select Scene media in Path tracing to render them."); }
        else if(!settings.path_trace && settings.unlit_inspection) {
            needs_settings=true; ImGui::TextWrapped("Unlit inspection is hiding the environment.");
            ImGui::BeginDisabled(model_.pending() || blocked);
            if(ImGui::Button("Show environment")) { auto value=settings; value.unlit_inspection=false; viewport_settings_changed(value); }
            ImGui::EndDisabled();
        } else if(!settings.path_trace && std::ranges::any_of(model_.snapshot()->scene.entities(),[](const auto& entity) {
            return std::ranges::any_of(entity.assets,[](const auto& a) { return a.kind==AssetKind::voxel; });
        })) { needs_settings=true; ImGui::TextWrapped("Voxel raster cannot combine air/clouds yet. Use Path trace with Scene media, or Unlit inspection."); }
        if(needs_settings && ImGui::Button("Open render settings")) ImGui::SetWindowFocus(settings.path_trace ? "Path tracing" : "View settings");
    }
    bool commit=false,discard=false;
    ImGui::BeginDisabled(blocked);
    ImGui::BeginDisabled(!draft->modified);
    if(ImGui::Button("Discard edits")) discard=true; mark("environment_revert"); help("environment.revert");
    if(!environment_error_.empty()) {
        if(ImGui::GetContentRegionAvail().x>ImGui::CalcTextSize("Retry changes").x+ImGui::GetStyle().FramePadding.x*2) ImGui::SameLine();
        if(ImGui::Button("Retry changes")) commit=true; mark("environment_apply"); help("environment.apply");
    }
    ImGui::EndDisabled();
    ImGui::SeparatorText("Environment");
    auto& e=draft->value;
    bool enabled=e.at("enabled").get<bool>();
    if(ImGui::Checkbox("Atmosphere",&enabled)) { e["enabled"]=enabled; draft->modified=true; commit=true; } mark("environment_enabled"); help("environment.enabled");
    bool clouds=e["clouds"]["enabled"].get<bool>();
    if(ImGui::Checkbox("Clouds",&clouds)) { e["clouds"]["enabled"]=clouds; draft->modified=true; commit=true; } mark("cloud_enabled"); help("environment.clouds.enabled");
    ImGui::TextUnformatted("Presets");
    for(const char* name:{"clear","sparse","overcast","sunset"}) {
        if(name!=std::string_view{"clear"} && ImGui::GetItemRectMax().x+ImGui::CalcTextSize(name).x+ImGui::GetStyle().FramePadding.x*2+ImGui::GetStyle().ItemSpacing.x<ImGui::GetCursorScreenPos().x+ImGui::GetContentRegionAvail().x) ImGui::SameLine();
        if(ImGui::Button(name)) { if(report("Environment preset",model_.preset_environment(name))) commit=true; }
        const auto id=std::string{"cloud_preset_"}+name; mark(id.c_str()); help("environment.preset");
    }
    auto finished=[&] { if(ImGui::IsItemDeactivatedAfterEdit()) commit=true; };
    auto scalar=[&](const char* label,Json& value,const std::string& setting,const char* format="%.6g") {
        ImGui::TextWrapped("%s",label); ImGui::SetNextItemWidth(-1);
        const auto id=std::string{"##"}+label;
        double v=value.get<double>(); if(ImGui::InputDouble(id.c_str(),&v,0,0,format)) { value=v; draft->modified=true; }
        finished(); help(setting);
    };
    auto vector=[&](const char* label,Json& value,const std::string& setting) {
        ImGui::TextWrapped("%s",label); ImGui::SetNextItemWidth(-1);
        const auto id=std::string{"##"}+label;
        double v[]{value[0].get<double>(),value[1].get<double>(),value[2].get<double>()};
        if(ImGui::InputScalarN(id.c_str(),ImGuiDataType_Double,v,3,nullptr,nullptr,"%.6g")) { value=Json::array({v[0],v[1],v[2]}); draft->modified=true; }
        finished(); help(setting);
    };
    auto slider=[&](const char* label,Json& value,float lo,float hi,const std::string& setting) {
        ImGui::TextUnformatted(label); ImGui::SetNextItemWidth(-1);
        float v=value.get<float>(); const auto id=std::string{"##"}+label;
        if(ImGui::SliderFloat(id.c_str(),&v,lo,hi,"%.3f",ImGuiSliderFlags_AlwaysClamp)) { value=v; draft->modified=true; }
        finished(); help(setting);
    };
    if(settings_help_.reveal("Environment","Sun")) ImGui::SetNextItemOpen(true);
    if(ImGui::CollapsingHeader("Sun",ImGuiTreeNodeFlags_DefaultOpen)) {
        auto& direction=e["sun"]["direction"];
        auto angles=sun_angles({direction[0].get<double>(),direction[1].get<double>(),direction[2].get<double>()});
        const auto set_direction=[&](SunAngles a) {
            const auto v=sun_direction(a); const auto candidate=Json::array({v.x(),v.y(),v.z()});
            if(candidate!=direction) { direction=candidate; draft->modified=true; }
        };
        const bool side_by_side=ImGui::GetContentRegionAvail().x>=ImGui::GetFontSize()*17;
        const float diameter=std::clamp(ImGui::GetContentRegionAvail().x,80.f,ImGui::GetFontSize()*8);
        const auto origin=ImGui::GetCursorScreenPos(); const float radius=diameter*.5f-14;
        const ImVec2 center{origin.x+diameter*.5f,origin.y+diameter*.5f};
        ImGui::InvisibleButton("Sun direction disk",{diameter,diameter}); mark("environment_sun_disk");
        const bool active=ImGui::IsItemActive();
        if(active && ImGui::IsMouseDown(0)) {
            const auto mouse=ImGui::GetIO().MousePos;
            const Vec2d point{(mouse.x-center.x)/radius,(mouse.y-center.y)/radius};
            if(ImGui::IsMouseDragging(0,0) || point.norm()<=1) {
                const auto old_direction=direction; angles=sun_from_disk(point,angles.azimuth); set_direction(angles);
                if(direction!=old_direction) ImGui::MarkItemEdited(ImGui::GetItemID());
            }
        }
        if(active && ImGui::IsKeyPressed(ImGuiKey_Escape)) { discard=true; ImGui::ClearActiveID(); }
        finished(); help("environment.sun.direction");
        auto* draw=ImGui::GetWindowDrawList();
        draw->AddCircleFilled(center,radius,ImGui::GetColorU32(ImGuiCol_FrameBg),64);
        draw->AddCircleFilled(center,radius*.5f,ImGui::GetColorU32(ImVec4{.2f,.55f,.9f,.18f}),64);
        draw->AddCircle(center,radius,ImGui::GetColorU32(ImGuiCol_Border),64);
        draw->AddCircle(center,radius*.5f,ImGui::GetColorU32(ImGuiCol_TextDisabled),64);
        draw->AddLine({center.x-radius,center.y},{center.x+radius,center.y},ImGui::GetColorU32(ImGuiCol_Border));
        draw->AddLine({center.x,center.y-radius},{center.x,center.y+radius},ImGui::GetColorU32(ImGuiCol_Border));
        const auto point=sun_disk(angles); const ImVec2 handle{center.x+float(point.x())*radius,center.y+float(point.y())*radius};
        draw->AddLine(center,handle,ImGui::GetColorU32(ImGuiCol_SliderGrabActive),2);
        draw->AddCircleFilled(handle,8,ImGui::GetColorU32(active ? ImVec4{1,.85f,.2f,1} : ImVec4{1,.64f,.02f,1}));
        draw->AddCircle(handle,9,ImGui::GetColorU32(ImVec4{.18f,.13f,.04f,1}),24);
        draw->AddText({center.x-10,origin.y},ImGui::GetColorU32(ImGuiCol_Text),"+Z");
        draw->AddText({center.x+radius-10,center.y},ImGui::GetColorU32(ImGuiCol_Text),"+X");
        draw->AddText({center.x-radius,center.y},ImGui::GetColorU32(ImGuiCol_Text),"-X");
        draw->AddText({center.x-10,center.y+radius-4},ImGui::GetColorU32(ImGuiCol_Text),"-Z");
        if(side_by_side) ImGui::SameLine();
        ImGui::BeginGroup();
        float azimuth=float(angles.azimuth),elevation=float(angles.elevation);
        ImGui::TextUnformatted("Azimuth (deg)"); ImGui::SetNextItemWidth(-1);
        if(ImGui::SliderFloat("##sun_azimuth",&azimuth,-180,180,"%.1f",ImGuiSliderFlags_AlwaysClamp)) { angles.azimuth=azimuth; set_direction(angles); } finished(); help("environment.sun.direction");
        ImGui::TextUnformatted("Elevation (deg)"); ImGui::SetNextItemWidth(-1);
        if(ImGui::SliderFloat("##sun_elevation",&elevation,-90,90,"%.1f",ImGuiSliderFlags_AlwaysClamp)) { angles.elevation=elevation; set_direction(angles); } finished(); mark("environment_sun_elevation"); help("environment.sun.direction");
        ImGui::EndGroup();
        ImGui::TextWrapped("Center +90 / inner ring 0 / edge -90 degrees. Drag the sun; Esc cancels. Ctrl+click an angle to type.");
        vector("Irradiance (linear RGB)",e["sun"]["irradiance"],"environment.sun.irradiance");
        if(settings_help_.reveal("Environment","Sun/Advanced")) ImGui::SetNextItemOpen(true);
        if(ImGui::TreeNode("Advanced sun")) {
            vector("Direction XYZ",direction,"environment.sun.direction"); mark("environment_direction");
            scalar("Angular radius (rad)",e["sun"]["angular_radius_rad"],"environment.sun.angular_radius_rad");
            ImGui::TreePop();
        }
    }
    if(settings_help_.reveal("Environment","Cloud physical parameters")) ImGui::SetNextItemOpen(true);
    if(ImGui::CollapsingHeader("Cloud physical parameters")) {
        auto& c=e["clouds"];
        slider("Coverage",c["coverage"],0,1,"environment.clouds.coverage");
        slider("Density",c["density"],0,10,"environment.clouds.density");
        scalar("Bottom height (m)",c["bottom_height_m"],"environment.clouds.bottom_height_m");
        scalar("Top height (m)",c["top_height_m"],"environment.clouds.top_height_m");
        ImGui::TextUnformatted("Wind X/Z (m/s)"); ImGui::SetNextItemWidth(-1);
        double wind[]{c["wind_velocity_mps"][0].get<double>(),c["wind_velocity_mps"][1].get<double>()};
        if(ImGui::InputScalarN("##cloud_wind",ImGuiDataType_Double,wind,2)) { c["wind_velocity_mps"]={wind[0],wind[1]}; draft->modified=true; } finished(); help("environment.clouds.wind_velocity_mps");
        if(settings_help_.reveal("Environment","Cloud physical parameters/Advanced")) ImGui::SetNextItemOpen(true);
        if(ImGui::TreeNode("Cloud shape and generation")) {
            for(const char* name:{"seed","weather_resolution","base_resolution","detail_resolution"}) {
                auto value=c[name].get<std::uint32_t>(); ImGui::TextUnformatted(name); ImGui::SetNextItemWidth(-1);
                if(ImGui::InputScalar((std::string{"##"}+name).c_str(),ImGuiDataType_U32,&value)) { c[name]=value; draft->modified=true; } finished(); help("environment.clouds."+std::string(name));
            }
            ImGui::Text("Generator version: %u",c["generator_version"].get<std::uint32_t>());
            for(const char* name:{"extinction_per_m","weather_scale_m","shape_scale_m","detail_scale_m","bottom_fade_fraction","top_fade_fraction"}) scalar(name,c[name],"environment.clouds."+std::string(name));
            ImGui::TreePop();
        }
    }
    if(settings_help_.reveal("Environment","Atmosphere parameters")) ImGui::SetNextItemOpen(true);
    if(ImGui::CollapsingHeader("Atmosphere parameters")) {
        auto& a=e["atmosphere"];
        for(const char* key:{"planet_center_m","rayleigh_scattering_per_m","mie_scattering_per_m","mie_extinction_per_m","absorption_extinction_per_m","ground_albedo"}) vector(key,a[key],"environment.atmosphere."+std::string(key));
        for(const char* key:{"ground_radius_m","atmosphere_height_m","rayleigh_scale_height_m","mie_scale_height_m","mie_anisotropy","absorption_center_height_m","absorption_half_width_m"}) scalar(key,a[key],"environment.atmosphere."+std::string(key));
    }
    ImGui::EndDisabled();
    // refresh() replaces drafts: no references into e/draft are used below this point.
    if(discard) { if(report("Environment discard",model_.revert_environment())) environment_error_.clear(); }
    else if(commit && draft->modified) {
        auto result=model_.apply_environment();
        if(result) environment_error_.clear(); else environment_error_=result.error().message;
        report("Environment change",std::move(result));
    }
    ImGui::Separator(); ImGui::TextUnformatted("VIEWPORT");
    auto candidate=model_.editor_viewport()->settings();
    float exposure=float(candidate.raster_exposure);
    ImGui::BeginDisabled(candidate.path_trace || model_.pending() || input_.active() || input_.navigating());
    if(ImGui::SliderFloat("Raster exposure",&exposure,.01f,16.f,"%.3f",ImGuiSliderFlags_Logarithmic|ImGuiSliderFlags_AlwaysClamp)) {
        candidate.raster_exposure=exposure; viewport_settings_changed(candidate);
    } help("viewport.raster_exposure");
    ImGui::EndDisabled();
    if(candidate.path_trace) ImGui::TextWrapped("Path tracing uses Exposure EV in its own panel.");
    if(settings_help_.reveal("Environment","Cloud render quality and time")) ImGui::SetNextItemOpen(true);
    if(ImGui::CollapsingHeader("Cloud render quality and time")) {
        const bool reference_available=std::uint64_t{preview.info().width}*preview.info().height<=4096;
        const bool mode_open=ImGui::BeginCombo("Mode",environment_settings_.mode==render::CloudRenderMode::temporal ? "Temporal (progressive)" : "Reference (small viewport)"); help("preview.mode");
        if(mode_open) {
            if(ImGui::Selectable("Temporal (progressive)",environment_settings_.mode==render::CloudRenderMode::temporal)) environment_settings_.mode=render::CloudRenderMode::temporal;
            ImGui::BeginDisabled(!reference_available);
            if(ImGui::Selectable("Reference (up to 4096 pixels)",environment_settings_.mode==render::CloudRenderMode::reference)) environment_settings_.mode=render::CloudRenderMode::reference;
            help("preview.mode",reference_available ? "" : "Reference requires a viewport of at most 4096 pixels.");
            ImGui::EndDisabled(); ImGui::EndCombo();
        }
        ImGui::InputDouble("Time (s)",&environment_settings_.time_seconds,0,0,"%.6f"); mark("cloud_time"); help("preview.time_seconds");
        if(ImGui::Button("+1 second")) environment_settings_.time_seconds+=1; mark("cloud_time_step"); help("preview.time_seconds");
        ImGui::Checkbox("Cloud shadows",&environment_settings_.cloud_shadows); mark("cloud_shadow"); help("preview.cloud_shadows");
        ImGui::InputScalarN("Shadow center (m)",ImGuiDataType_Double,environment_settings_.shadow_center_m.data(),3); help("preview.shadow_center_m");
        ImGui::InputDouble("Shadow extent (m)",&environment_settings_.shadow_extent_m); help("preview.shadow_extent_m");
        ImGui::InputScalar("Shadow resolution",ImGuiDataType_U32,&environment_settings_.shadow_resolution); help("preview.shadow_resolution");
        ImGui::TextWrapped("Temporal shows a complete first frame, then refines while stationary. Reference requires a viewport of at most 4096 pixels. Time stays fixed until edited.");
    }
    ImGui::TextWrapped("Environment: near 1 m / far 10000 m. Disabled: near 0.05 m.");
    ImGui::End();
}
}
