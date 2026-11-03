#pragma once
#include "SettingsCatalog.hpp"
#include <dk/services/SceneService.hpp>
#include <imgui.h>
#include <array>
#include <map>
namespace dk::editor::detail {
class SettingsHelpUi final {
public:
    void begin_frame(const CommandCatalogSnapshot&,SettingsContext,DocumentId);
    void open() { open_=true; focus_search_=true; }
    void panel(const char* name);
    bool reveal(std::string_view panel,std::string_view section) const;
    void item(std::string_view id,std::string_view reason={});
    void unavailable_panel(const char* panel);
    // Popup-local lens edits affect its disabled hints before they are committed.
    void camera_clip_hint(bool automatic) { context_.automatic_clip=automatic; }
    // Returns an explicit request to insert a template; the caller owns Console protection.
    std::optional<std::string> draw(bool console_empty,bool composing,std::map<std::string,ImVec2>& controls);
    void feedback(std::string message) { status_=std::move(message); }
    [[nodiscard]] const std::string& located() const { return located_; }
    [[nodiscard]] const std::string& tooltip() const { return tooltip_; }
    [[nodiscard]] const std::string& selected() const { return selected_; }
    [[nodiscard]] bool is_open() const { return open_; }
    [[nodiscard]] const std::vector<SettingInfo>& entries() const { return entries_; }
    [[nodiscard]] const std::map<std::string,ImVec2>& positions() const { return positions_; }
private:
    const SettingInfo* find(std::string_view) const;
    void describe(const SettingInfo&,std::string_view reason);
    void locate(const SettingInfo&);
    std::vector<SettingInfo> entries_;
    std::map<std::string,ImVec2> positions_;
    CommandCatalogVersion catalog_;
    DocumentId document_;
    SettingsContext context_;
    std::array<char,257> query_{};
    std::string selected_,pending_,highlight_,located_,tooltip_,status_;
    double highlight_until_=0;
    bool open_=false,focus_search_=false,focus_target_=false;
};
}
