#pragma once
#include "Viewport.hpp"
#include "ViewportInput.hpp"
#include <map>

namespace dk::editor::detail {
enum class PendingAction { none, open, close };
class WorkbenchUi final {
public:
    explicit WorkbenchUi(Workspace& model,bool fixture);
    void draw(Viewport&,bool srgb);
    void request_close();
#ifdef DK_EDITOR_PATHTRACING
    [[nodiscard]] const PathPreviewSettings& path_settings() const { return path_settings_; }
    [[nodiscard]] bool path_mode() const { return path_mode_; }
#endif
    void cancel_interaction() { input_.cancel(); simulation_navigation_=-1; }
    [[nodiscard]] bool closing() const { return closing_; }
    [[nodiscard]] const std::map<std::string,ImVec2>& controls() const { return controls_; }
    [[nodiscard]] unsigned errors() const { return errors_; }
    [[nodiscard]] const ViewportInput& input() const { return input_; }
    [[nodiscard]] const SimulationState& displayed_simulation() const { return displayed_simulation_; }
    [[nodiscard]] const Camera& simulation_camera() const { return simulation_camera_; }
private:
    void mark(const char* id);
    bool report(const char*,Result<void>);
    void request(PendingAction);
    void perform();
    void toolbar();
    void hierarchy();
    void inspector();
    void assets();
    void environment(const Viewport&);
#ifdef DK_EDITOR_PATHTRACING
    void path_panel(Viewport&);
    PathPreviewSettings path_settings_;
    bool path_mode_=false;
#endif
    void console();
    void simulation();
    void simulation_results(const SimulationRunState*);
    bool simulation_view(Viewport&,bool visible);
    void frame_simulation(const ClothConfig&,double aspect);
    void simulation_navigation();
    void confirmation();
    Workspace& model_;
    ViewportInput input_;
    std::string manifest_;
    std::map<std::string,ImVec2> controls_;
    std::vector<std::string> log_;
    std::string viewport_error_;
    std::string simulation_error_;
    std::string simulation_sample_error_,simulation_output_;
    int simulation_export_width_=640,simulation_export_height_=480;
    std::optional<SimulationId> simulation_panel_run_;
    bool simulation_panel_initialized_=false;
    SimulationState displayed_simulation_{SimulationMode::edit,{}};
    Camera simulation_camera_;
    std::optional<SimulationId> simulation_camera_run_;
    std::optional<ClothConfig> simulation_cloth_;
    bool simulation_visible_=true;
    int simulation_navigation_=-1;
    PendingAction pending_ = PendingAction::none;
    bool popup_ = false,closing_ = false,layout_ = false,scroll_log_ = true;
    unsigned errors_ = 0;
    float exposure_=1;
    render::EnvironmentRenderSettings environment_settings_=[] { render::EnvironmentRenderSettings s; s.mode=render::CloudRenderMode::temporal; return s; }();
};
}
