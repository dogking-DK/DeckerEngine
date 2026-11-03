#pragma once
#include <dk/commands/CommandRegistry.hpp>
namespace dk::editor::detail {
struct SettingInfo {
    std::string id,panel,section,label,aliases,command,path,explanation,impact;
    std::string help,command_template,search_text;
    bool command_available=false;
};
struct SettingsContext {
    bool scene=false,selection=false,inspector_draft=false,environment_draft=false,gizmo=false,navigating=false;
    bool simulation_view=false,path_available=false,path_mode=false,restir=false,scene_media=false;
    bool orthographic=false,automatic_clip=true,camera_popup=false,simulation_gpu=false,simulation_run=false;
    bool simulation_paused=false,simulation_terminal=false,simulation_stopping=false,simulation_fault=false;
    bool composing=false,small_cloud_view=false;
    std::string path_reason;
};
struct SettingSearch { std::vector<std::size_t> indices; bool truncated=false; std::string error; };
std::vector<SettingInfo> build_settings_catalog(const CommandCatalogSnapshot&);
SettingSearch search_settings(const std::vector<SettingInfo>&,std::string_view);
std::string setting_unavailable(const SettingInfo&,const SettingsContext&);
}
