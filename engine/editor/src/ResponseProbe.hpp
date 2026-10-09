#pragma once
#include "EditorSupport.hpp"
#include <dk/editor/Workspace.hpp>
#include <dk/io/File.hpp>
#include <chrono>
#include <cstdio>

namespace dk::editor::detail {
class ShutdownProbe final {
public:
    explicit ShutdownProbe(bool enabled) : enabled_(enabled) {}
    void begin() { if (enabled_) start_=std::chrono::steady_clock::now(); }
    void mark(const char* phase) const noexcept {
        if (start_) {
            const auto now=std::chrono::steady_clock::now();
            std::printf("shutdown phase=%s elapsed_ns=%lld at_ns=%lld\n",phase,static_cast<long long>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(now-*start_).count()),static_cast<long long>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count()));
        }
    }
private:
    bool enabled_;
    std::optional<std::chrono::steady_clock::time_point> start_;
};
struct ShutdownPhase {
    const ShutdownProbe& probe;
    const char* name;
    ~ShutdownPhase() { probe.mark(name); }
};
// Disposable acceptance only: sample the actual event loop, never a timer thread.
class ResponseProbe final {
public:
    explicit ResponseProbe(std::filesystem::path root) : root_(std::move(root)) { samples_.reserve(120000); }
    void tick(const SimulationState& state) {
        if (!ready_) return;
        if (samples_.size()==120000) throw std::runtime_error("GUI response probe sample limit exceeded");
        Sample s{now(),"edit",0,0};
        if (state.run && state.run->task) {
            s.status=simulation_task_status_name(state.run->task->status);
            s.steps=state.run->task->completed_steps; s.submitted=state.run->task->submitted_steps;
        }
        samples_.push_back(s);
    }
    void presented() {
        if (!ready_ && ++frames_==30) {
            save("response-ready.json",{{"schema",1},{"ready_ns",now()}});
            ready_=true;
        }
    }
    void finish() const {
        Json samples=Json::array();
        for (const auto& s:samples_) samples.push_back({s.time,s.status,s.steps,s.submitted});
        save("response-heartbeat.json",{{"schema",1},{"columns",{"time_ns","status","completed_steps","submitted_steps"}},
            {"samples",std::move(samples)}});
    }
private:
    static std::int64_t now() { return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count(); }
    void save(const char* name,const Json& value) const {
        const auto text=value.dump();
        check(write_file_bytes_atomic(root_/name,std::as_bytes(std::span{text.data(),text.size()})));
    }
    struct Sample { std::int64_t time; std::string_view status; std::uint64_t steps,submitted; };
    std::filesystem::path root_;
    std::vector<Sample> samples_;
    unsigned frames_=0;
    bool ready_=false;
};
}
