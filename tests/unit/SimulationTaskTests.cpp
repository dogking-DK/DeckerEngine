#include "AsyncSimulation.hpp"
#include <catch2/catch_test_macros.hpp>
#include <atomic>
#include <thread>

using namespace dk;
using namespace dk::detail;
using namespace std::chrono_literals;
namespace {
template<class F> void until(F condition) {
    const auto end=std::chrono::steady_clock::now()+5s;
    while (!condition() && std::chrono::steady_clock::now()<end) std::this_thread::sleep_for(1ms);
    REQUIRE(condition());
}
struct Gate {
    std::atomic<bool> initialize=false, submit=false, complete=false, entered=false, submitting=false, destroyed=false;
    std::atomic<unsigned> submits=0, polls=0, max_batch=0;
    int fail=0;
    std::thread::id initialized_on, destroyed_on;
};
struct Fake final : SimulationTaskBackend {
    std::shared_ptr<Gate> gate;
    std::uint64_t steps=0;
    explicit Fake(std::shared_ptr<Gate> g) : gate(std::move(g)) {}
    ~Fake() override { gate->destroyed_on=std::this_thread::get_id(); gate->destroyed=true; }
    Result<void> initialize() override {
        gate->initialized_on=std::this_thread::get_id(); gate->entered=true;
        while (!gate->initialize) std::this_thread::sleep_for(1ms);
        if (gate->fail==4) throw std::bad_alloc{};
        if (gate->fail==1) return std::unexpected(Error{ErrorCode::not_supported,"init failed"});
        return {};
    }
    Result<void> submit(std::int64_t, std::uint32_t count) override {
        gate->submitting=true; ++gate->submits;
        gate->max_batch=std::max(gate->max_batch.load(),count);
        while (!gate->submit) std::this_thread::sleep_for(1ms);
        if (gate->fail==2) return std::unexpected(Error{ErrorCode::invalid_argument,"pre-submit failure"});
        steps+=count; return {};
    }
    Result<bool> poll() override {
        ++gate->polls;
        if (gate->fail==3) return std::unexpected(Error{ErrorCode::invalid_state,"post-submit device failure"});
        return gate->complete.load();
    }
    std::uint64_t submitted() const override { return steps; }
    Result<XpbdSnapshot> read() override {
        if (gate->fail==5) return std::unexpected(Error{ErrorCode::invalid_state,"non-finite readback"});
        return XpbdSolver::cloth()->snapshot();
    }
};
struct Fixture {
    std::shared_ptr<Gate> gate=std::make_shared<Gate>();
    std::unique_ptr<AsyncSimulation> task;
    void start(std::uint32_t count=100) {
        task=std::make_unique<AsyncSimulation>([g=gate] { return std::make_unique<Fake>(g); },10000000,count,8);
    }
    ~Fixture() { gate->initialize=true; gate->submit=true; gate->complete=true; task.reset(); }
};
}
TEST_CASE("finite task pause during initialization then cancel claimed batch waits for GPU completion") {
    Fixture f; f.start(); until([&] { return f.gate->entered.load(); });
    REQUIRE(f.task->state().status==SimulationTaskStatus::initializing);
    REQUIRE(f.task->pause()); REQUIRE(f.task->state().status==SimulationTaskStatus::pausing);
    REQUIRE_FALSE(f.task->resume()); REQUIRE_FALSE(f.task->read());
    f.gate->initialize=true;
    until([&] { return f.task->state().status==SimulationTaskStatus::paused; });
    REQUIRE(f.task->state().completed_steps==0); REQUIRE(f.task->read());
    REQUIRE(f.task->resume()); until([&] { return f.gate->submitting.load(); });
    REQUIRE(f.task->cancel()); REQUIRE(f.task->state().status==SimulationTaskStatus::cancelling);
    REQUIRE_FALSE(f.task->resume()); REQUIRE_FALSE(f.task->pause());
    f.gate->submit=true;
    until([&] { return f.task->state().submitted_steps==8; });
    REQUIRE(f.task->state().completed_steps==0);
    REQUIRE(f.task->state().batch_active); REQUIRE(f.gate->submits==1);
    f.gate->complete=true;
    until([&] { return f.task->state().status==SimulationTaskStatus::cancelled; });
    REQUIRE(f.task->state().completed_steps==8); REQUIRE(f.gate->submits==1);
    REQUIRE(f.task->cancel()); REQUIRE(f.task->read()); REQUIRE_FALSE(f.task->resume());
    f.task->stop(); until([&] { return f.task->closed(); });
    REQUIRE(f.gate->destroyed); REQUIRE(f.gate->initialized_on==f.gate->destroyed_on);
    REQUIRE(f.gate->initialized_on!=std::this_thread::get_id());
}
TEST_CASE("finite task pause after submit publishes a stable boundary before resume") {
    Fixture f; f.gate->initialize=true; f.gate->submit=true; f.start(17);
    until([&] { return f.task->state().submitted_steps==8; });
    REQUIRE(f.task->pause()); REQUIRE(f.task->state().status==SimulationTaskStatus::pausing);
    f.gate->complete=true;
    until([&] { return f.task->state().status==SimulationTaskStatus::paused; });
    for (int i=0;i<20;++i) { REQUIRE(f.task->state().completed_steps==8); REQUIRE(f.gate->submits==1); }
    REQUIRE(f.task->resume());
    until([&] { return f.task->state().status==SimulationTaskStatus::succeeded; });
    REQUIRE(f.task->state().submitted_steps==17); REQUIRE(f.task->state().completed_steps==17);
    REQUIRE(f.gate->submits==3); REQUIRE(f.gate->max_batch==8);
    REQUIRE(f.task->cancel()); REQUIRE(f.task->state().status==SimulationTaskStatus::succeeded);
    REQUIRE_FALSE(f.task->resume());
}
TEST_CASE("finite task cancellation wins completion race without rolling back the final batch") {
    Fixture f; f.gate->initialize=true; f.gate->submit=true; f.start(8);
    until([&] { return f.task->state().submitted_steps==8; });
    REQUIRE(f.task->cancel()); f.gate->complete=true;
    until([&] { return f.task->state().status==SimulationTaskStatus::cancelled; });
    REQUIRE(f.task->state().completed_steps==8);
}
TEST_CASE("finite task ordinary failures freeze submitted and completed counts separately") {
    for (int failure=1;failure<=3;++failure) {
        Fixture f; f.gate->fail=failure; f.gate->initialize=true; f.gate->submit=true; f.start();
        until([&] { return f.task->state().status==SimulationTaskStatus::failed; });
        REQUIRE(f.task->state().error.has_value()); REQUIRE(f.task->state().completed_steps==0);
        REQUIRE(f.task->state().submitted_steps==(failure==3 ? 8 : 0));
        REQUIRE_FALSE(f.task->resume()); REQUIRE_FALSE(f.task->read());
        REQUIRE(f.task->cancel()); REQUIRE(f.task->state().status==SimulationTaskStatus::failed);
        f.task->stop(); until([&] { return f.task->closed(); }); REQUIRE(f.gate->destroyed);
    }
}
TEST_CASE("finite task infrastructure exceptions remain fatal and close reclaims the worker") {
    Fixture f; f.gate->fail=4; f.gate->initialize=true; f.start();
    until([&] { return f.task->closed(); });
    REQUIRE_THROWS_AS(f.task->state(),std::bad_alloc); REQUIRE(f.gate->destroyed);
}
TEST_CASE("finite task stop during initialization waits for backend ownership to end without submitting") {
    Fixture f; f.start(); until([&] { return f.gate->entered.load(); });
    f.task->stop(); REQUIRE(f.task->state().status==SimulationTaskStatus::stopping);
    REQUIRE_FALSE(f.task->closed()); REQUIRE_FALSE(f.task->cancel());
    f.gate->initialize=true;
    until([&] { return f.task->closed(); }); REQUIRE(f.gate->destroyed); REQUIRE(f.gate->submits==0);
}
TEST_CASE("finite task cancel during initialization reaches a stable zero-step result") {
    Fixture f; f.start(); until([&] { return f.gate->entered.load(); });
    REQUIRE(f.task->cancel()); REQUIRE(f.task->state().status==SimulationTaskStatus::cancelling);
    REQUIRE_FALSE(f.task->read());
    f.gate->initialize=true;
    until([&] { return f.task->state().status==SimulationTaskStatus::cancelled; });
    REQUIRE(f.task->state().initialized); REQUIRE(f.task->state().completed_steps==0);
    REQUIRE(f.gate->submits==0); REQUIRE(f.task->read());
}
TEST_CASE("finite task Stop keeps submitted work alive until completion before destroying its backend") {
    Fixture f; f.gate->initialize=true; f.gate->submit=true; f.start();
    until([&] { return f.task->state().submitted_steps==8; });
    f.task->stop(); REQUIRE(f.task->state().status==SimulationTaskStatus::stopping);
    REQUIRE_FALSE(f.task->closed()); REQUIRE_FALSE(f.gate->destroyed);
    REQUIRE_FALSE(f.task->read()); REQUIRE_FALSE(f.task->resume());
    f.gate->complete=true;
    until([&] { return f.task->closed(); });
    REQUIRE(f.task->state().completed_steps==8); REQUIRE(f.gate->submits==1);
    REQUIRE(f.gate->destroyed); REQUIRE(f.gate->initialized_on==f.gate->destroyed_on);
}
TEST_CASE("finite task diagnostic failure freezes a completed experiment without changing its counts") {
    Fixture f; f.gate->fail=5; f.gate->initialize=true; f.gate->submit=true; f.gate->complete=true; f.start(8);
    until([&] { return f.task->state().status==SimulationTaskStatus::succeeded; });
    REQUIRE_FALSE(f.task->read()); REQUIRE(f.task->state().status==SimulationTaskStatus::failed);
    REQUIRE(f.task->state().submitted_steps==8); REQUIRE(f.task->state().completed_steps==8);
    REQUIRE_FALSE(f.task->resume()); REQUIRE_FALSE(f.task->read());
}
