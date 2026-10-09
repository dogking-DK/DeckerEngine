#pragma once
#include <dk/runtime/Runtime.hpp>
#include <dk/editor/TransformEdit.hpp>
#ifdef _WIN32
#include <dk/automation/IpcServer.hpp>
#endif

namespace dk::editor {
// UI state only; all durable mutations go through Runtime commands on its owner thread.
struct InspectorDraft {
    EditGuard guard;
    EntityData entity;
    bool modified = false;
};
class Workspace final {
public:
    [[nodiscard]] static Result<std::unique_ptr<Workspace>> create(const std::filesystem::path& root);
    [[nodiscard]] Result<void> open(const std::filesystem::path& manifest, bool discard = false);
    [[nodiscard]] Result<void> refresh();
    [[nodiscard]] Result<void> select(std::optional<EntityId> id);
    [[nodiscard]] Result<void> apply();
    [[nodiscard]] Result<void> revert();
    [[nodiscard]] Result<void> save();
    [[nodiscard]] Result<void> undo();
    [[nodiscard]] Result<void> redo();
    [[nodiscard]] Result<TransformEdit> begin_transform() const;
    [[nodiscard]] Result<void> commit_transform(const TransformEdit&);
    void pump();
    [[nodiscard]] Result<void> set_simulation_gpu_device(std::shared_ptr<const graphics::Device> device) {
        return runtime_->set_simulation_gpu_device(std::move(device));
    }
    [[nodiscard]] Result<void> request_shutdown();
    [[nodiscard]] SimulationState simulation_state() const { return runtime_->simulation_state(); }
    [[nodiscard]] bool stopping() const { return runtime_->stopping(); }
#ifdef _WIN32
    [[nodiscard]] Result<void> start_ipc(std::string_view name);
    void close_ipc();
#endif
    [[nodiscard]] const SceneReadSnapshot* snapshot() const { return snapshot_ ? &*snapshot_ : nullptr; }
    [[nodiscard]] const std::filesystem::path& manifest() const { return manifest_; }
    [[nodiscard]] InspectorDraft* draft() { return draft_ ? &*draft_ : nullptr; }
    [[nodiscard]] const InspectorDraft* draft() const { return draft_ ? &*draft_ : nullptr; }
    [[nodiscard]] bool pending() const { return draft_ && draft_->modified; }
    [[nodiscard]] bool dirty() const { return pending() || (snapshot_ && snapshot_->state.dirty); }
    [[nodiscard]] std::optional<EntityId> selection() const { return selection_; }
    [[nodiscard]] HistoryStatus history() const { return history_; }
private:
    explicit Workspace(std::unique_ptr<Runtime> runtime) : runtime_(std::move(runtime)) {}
    [[nodiscard]] Result<Json> call(std::string_view method, Json parameters = Json::object());
    [[nodiscard]] Result<void> history_step(std::string_view method);
    [[nodiscard]] Json guard() const;
    std::unique_ptr<Runtime> runtime_;
    std::optional<SceneReadSnapshot> snapshot_;
    std::optional<EntityId> selection_;
    std::optional<InspectorDraft> draft_;
    std::filesystem::path manifest_;
    HistoryStatus history_{};
#ifdef _WIN32
    std::unique_ptr<IpcServer> ipc_;
#endif
};
} // namespace dk::editor
