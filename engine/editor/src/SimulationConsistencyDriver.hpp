#pragma once
#include "WorkbenchUi.hpp"

namespace dk::editor::detail {
// Disposable acceptance only: UI actions and external IPC alternate at presented checkpoints.
class SimulationConsistencyDriver final {
public:
    explicit SimulationConsistencyDriver(const std::filesystem::path& root);
    void input(WorkbenchUi&);
    void presented(const Workspace&,const WorkbenchUi&,Viewport&);
    [[nodiscard]] bool done() const { return done_; }
private:
    [[nodiscard]] std::string checkpoint() const;
    std::filesystem::path directory_;
    unsigned backend_=0,stage_=0,tick_=0,stable_frames_=0;
    bool ready_=false,waiting_=false,done_=false;
    ImVec2 pointer_{};
    std::optional<SceneSnapshot> initial_scene_,edited_scene_;
    std::optional<SimulationId> run_id_;
    DocumentId document_id_;
    std::uint64_t initial_revision_=0,pause_steps_=0,pause_pixels_=0;
    std::size_t initial_history_=0;
};
}
