#pragma once
#include <dk/commands/CommandRegistry.hpp>
#include <dk/services/SceneService.hpp>
#include <dk/services/SimulationService.hpp>
#include <dk/runtime/RuntimeEvents.hpp>
#ifdef DK_RUNTIME_ASSETS
#include <dk/services/AsyncAssetService.hpp>
#endif

#ifdef DK_RUNTIME_CAPTURE
#include <dk/services/CaptureService.hpp>
#endif

namespace dk
{
struct TaskIdTag;
using TaskId = StableId<TaskIdTag>;
struct CommandExecution
{
    TaskId task_id;
    Result<Json> result;
};
struct TaskRecord
{
    TaskId id;
    std::string method;
    bool succeeded;
    std::optional<ErrorCode> error_code;
    std::optional<DocumentState> document;
};
class Runtime final
{
  public:
    [[nodiscard]] static Result<std::unique_ptr<Runtime>> create(const std::filesystem::path &root);
    [[nodiscard]] Result<CommandExecution> dispatch(std::string_view method, const Json &parameters,
                                                    bool auto_guard = false);
    [[nodiscard]] bool has_command(std::string_view method) const;
    [[nodiscard]] std::shared_ptr<RuntimeEvents> events() const { return events_; }
    // Immutable owner-thread copy, checked against the active session and revision.
    [[nodiscard]] Result<SceneReadSnapshot> read_scene(EditGuard guard) const;
    [[nodiscard]] Result<PlaySceneSnapshot> read_play_scene(SimulationId run_id) const;
    [[nodiscard]] Result<PlayParticleSnapshot> read_play_particles(SimulationId run_id) const;
    void pump();
    [[nodiscard]] Result<void> set_simulation_gpu_device(std::shared_ptr<const graphics::Device> device) {
        return simulation_.set_gpu_device(std::move(device));
    }
    [[nodiscard]] SimulationState simulation_state() const { return simulation_.state(); }
#ifdef DK_SIMULATION_GPU
    [[nodiscard]] Result<void> request_simulation_preview(SimulationId id,const SimulationView& view) { return simulation_.request_preview(id,view); }
    [[nodiscard]] Result<std::shared_ptr<SimulationPreview>> take_simulation_preview(SimulationId id) { return simulation_.take_preview(id); }
#endif
    [[nodiscard]] std::chrono::steady_clock::time_point next_pump_deadline(std::chrono::steady_clock::time_point fallback) const;
    [[nodiscard]] bool stopping() const noexcept
    {
        return stopping_;
    }

  private:
    explicit Runtime(std::unique_ptr<SceneService> service) : service_{std::move(service)} {}
    std::unique_ptr<SceneService> service_;
    SimulationService simulation_;
    std::shared_ptr<RuntimeEvents> events_ = std::make_shared<RuntimeEvents>();
#ifdef DK_RUNTIME_ASSETS
    std::unique_ptr<AsyncAssetService> assets_;
#endif
#ifdef DK_RUNTIME_CAPTURE
    std::unique_ptr<CaptureService> captures_;
#endif
#if defined(DK_RUNTIME_CAPTURE) || defined(DK_RUNTIME_ASSETS)
    [[nodiscard]] Result<JobSnapshot> job(JobId) const;
    [[nodiscard]] Result<JobWait> wait_job(JobId,std::chrono::milliseconds);
    [[nodiscard]] Result<JobCancel> cancel_job(JobId);
#endif
    CommandRegistry commands_;
    bool dispatching_ = false;
    bool stopping_ = false;
    std::vector<TaskRecord> tasks_;
    [[nodiscard]] Result<void> register_runtime_commands();
};
} // namespace dk
