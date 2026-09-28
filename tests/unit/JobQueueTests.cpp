#include <dk/jobs/JobQueue.hpp>
#include <dk/memory/MemorySystem.hpp>
#include <dk/memory/SmartPtr.hpp>
#include <dk/memory/Containers.hpp>
#include <catch2/catch_test_macros.hpp>
#include <atomic>
#include <condition_variable>
#include <latch>
#include <thread>

using namespace dk;
namespace {
struct Environment {
    memory::MemorySystem system = [] { auto r = memory::MemorySystem::create(); if (!r) throw std::bad_alloc{}; return std::move(*r); }();
    memory::ResourceHandle heap = *system.create_heap();
    memory::ResourceHandle queue_heap = *system.create_heap({"jobs", memory::DomainCategory::jobs});
    memory::ThreadContext context{system, heap};
    memory::ExecutionScope scope{context, heap};
};
JobQueue::Payload payload() { return memory::make_shared<int>(42); }
Result<std::string> publish(JobId, const JobQueue::Payload&) { return "published"; }
JobSnapshot complete(JobQueue& queue, JobId id, const JobQueue::Consumer& consumer = publish)
{
    const auto deadline = JobQueue::Clock::now() + std::chrono::seconds(5);
    for (;;) {
        const auto sequence = queue.change_sequence();
        queue.drain(consumer);
        auto state = queue.query(id); REQUIRE(state);
        if (terminal(state->state)) return *state;
        REQUIRE(JobQueue::Clock::now() < deadline);
        queue.wait_change(sequence, deadline);
    }
}
}
TEST_CASE("queue captures owning domain and awaits owner publication")
{
    Environment env; JobQueue queue(env.queue_heap);
    auto other = *env.system.create_heap({"asset", memory::DomainCategory::assets});
    std::latch done{1}; JobId id;
    {
        memory::DomainScope domain(other);
        auto result = queue.submit([&](std::stop_token) -> Result<JobQueue::Payload> {
            auto v = memory::make_shared<Vector<int>>(4, 9);
            if (v->get_allocator().resource() != other) throw std::runtime_error("wrong domain");
            auto scratch = memory::scratch_vector<int>(); scratch.resize(20);
            done.count_down(); return v;
        }, 32);
        REQUIRE(result); id = *result;
    }
    done.wait();
    auto waiting = queue.wait(id, JobQueue::Clock::now()); REQUIRE(waiting);
    CHECK(waiting->timed_out); CHECK(waiting->job.state == JobState::running);
    std::shared_ptr<const Vector<int>> retained;
    auto state = complete(queue, id, [&](JobId job, const JobQueue::Payload& value) -> Result<std::string> {
        CHECK(queue.query(job)->state == JobState::running); // Consumer runs outside mutex.
        retained = std::static_pointer_cast<const Vector<int>>(value); return "ready";
    });
    CHECK(state.state == JobState::succeeded); CHECK(state.summary == "ready");
    queue.close(); CHECK(retained->at(0) == 9);
    CHECK_FALSE(queue.request_cancel(id)->accepted);
    CHECK_FALSE(queue.submit([](std::stop_token) -> Result<JobQueue::Payload> { return payload(); }, 1));
}
TEST_CASE("queue bounds queued active input and terminal retention without invoking rejected work")
{
    Environment env; JobQueue queue(env.queue_heap, {1, 2, 1, 8});
    std::latch started{1}; std::atomic<int> invoked{0};
    auto first = queue.submit([&](std::stop_token stop) -> Result<JobQueue::Payload> {
        started.count_down(); std::mutex m; std::condition_variable_any cv; std::unique_lock lock(m);
        cv.wait(lock, stop, [] { return false; }); return payload();
    }, 4); REQUIRE(first); started.wait();
    auto work = [&](std::stop_token) -> Result<JobQueue::Payload> { ++invoked; return payload(); };
    CHECK_FALSE(queue.submit(work, 5)); // bytes
    auto second = queue.submit(work, 4); REQUIRE(second);
    CHECK_FALSE(queue.submit(work, 1)); // queue/active
    auto cancelled = queue.request_cancel(*second); REQUIRE(cancelled); CHECK(cancelled->accepted);
    CHECK(cancelled->job.state == JobState::cancelled); CHECK(invoked == 0);
    REQUIRE(queue.request_cancel(*first)); CHECK(complete(queue, *first).state == JobState::cancelled);
    CHECK_FALSE(queue.query(*second));
    auto third = queue.submit(work, 8); REQUIRE(third); CHECK(complete(queue, *third).state == JobState::succeeded);
    CHECK_FALSE(queue.query(*first)); CHECK(invoked == 1);
}
TEST_CASE("queue active completions remain bounded and accepted cancellation prevents publishing")
{
    Environment env; JobQueue queue(env.queue_heap, {4, 1, 4, 8}); std::latch returned{1};
    auto id = queue.submit([&](std::stop_token) -> Result<JobQueue::Payload> { returned.count_down(); return payload(); }, 4);
    REQUIRE(id); returned.wait();
    CHECK_FALSE(queue.submit([](std::stop_token) -> Result<JobQueue::Payload> { return payload(); }, 1));
    REQUIRE(queue.request_cancel(*id)); int published = 0;
    CHECK(complete(queue, *id, [&](JobId, const auto&) -> Result<std::string> { ++published; return "wrong"; }).state == JobState::cancelled);
    CHECK(published == 0);
}
TEST_CASE("queue business failure differs from infrastructure exception")
{
    Environment env; JobQueue queue(env.queue_heap);
    auto id = queue.submit([](std::stop_token) -> Result<JobQueue::Payload> {
        return std::unexpected(Error{ErrorCode::io_error, "bad source"});
    }, 1); REQUIRE(id); auto state = complete(queue, *id);
    CHECK(state.state == JobState::failed); REQUIRE(state.error); CHECK(state.error->code == ErrorCode::io_error);
    auto bad = queue.submit([](std::stop_token) -> Result<JobQueue::Payload> { throw std::bad_alloc{}; }, 1); REQUIRE(bad);
    const auto deadline = JobQueue::Clock::now() + std::chrono::seconds(5);
    bool threw = false;
    while (!threw && JobQueue::Clock::now() < deadline) {
        const auto sequence = queue.change_sequence();
        try { queue.rethrow_failure(); } catch (const std::bad_alloc&) { threw = true; }
        if (!threw) queue.wait_change(sequence, deadline);
    }
    CHECK(threw); CHECK_THROWS_AS(queue.drain(publish), std::bad_alloc); queue.close();
    CHECK(queue.query(*bad)->state == JobState::cancelled);
}
TEST_CASE("queue closes cooperative running and queued work and wakes waiters")
{
    Environment env; JobQueue queue(env.queue_heap); std::latch started{1}; std::atomic<int> calls{0};
    auto id = queue.submit([&](std::stop_token stop) -> Result<JobQueue::Payload> {
        std::mutex m; std::condition_variable_any cv; std::unique_lock lock(m); started.count_down();
        cv.wait(lock, stop, [] { return false; }); return payload();
    }, 1); REQUIRE(id); started.wait();
    auto next = queue.submit([&](std::stop_token) -> Result<JobQueue::Payload> { ++calls; return payload(); }, 1); REQUIRE(next);
    std::optional<JobWait> waited;
    std::jthread waiter([&] { waited = *queue.wait(*id, JobQueue::Clock::now() + std::chrono::seconds(5)); });
    queue.close(); waiter.join(); CHECK(calls == 0); REQUIRE(waited); CHECK_FALSE(waited->timed_out);
    CHECK(waited->job.state == JobState::cancelled); CHECK(queue.query(*next)->state == JobState::cancelled);
}
TEST_CASE("queue releases worker context between systems and after system closing")
{
    Environment env; JobQueue queue(env.queue_heap); JobId id;
    auto other = memory::MemorySystem::create(); REQUIRE(other); auto heap = *other->create_heap();
    {
        memory::ThreadContext context(*other, heap); memory::ExecutionScope scope(context, heap);
        auto r = queue.submit([](std::stop_token) -> Result<JobQueue::Payload> { return payload(); }, 1); REQUIRE(r); id = *r;
    }
    CHECK(complete(queue, id).state == JobState::succeeded);
    CHECK(other->try_close().closed());
    auto next = queue.submit([](std::stop_token) -> Result<JobQueue::Payload> { return payload(); }, 1); REQUIRE(next);
    CHECK(complete(queue, *next).state == JobState::succeeded);
}
TEST_CASE("queue never starts work from a retired submitting system")
{
    Environment env; JobQueue queue(env.queue_heap); std::latch started{1};
    auto first = queue.submit([&](std::stop_token stop) -> Result<JobQueue::Payload> {
        std::mutex m; std::condition_variable_any cv; std::unique_lock lock(m); started.count_down();
        cv.wait(lock, stop, [] { return false; }); return payload();
    }, 1); REQUIRE(first); started.wait();
    auto other = memory::MemorySystem::create(); REQUIRE(other); auto heap = *other->create_heap();
    int calls = 0; JobId queued;
    {
        memory::ThreadContext context(*other, heap); memory::ExecutionScope scope(context, heap);
        auto id = queue.submit([&](std::stop_token) -> Result<JobQueue::Payload> { ++calls; return payload(); }, 1);
        REQUIRE(id); queued = *id;
    }
    CHECK(other->try_close().closed()); REQUIRE(queue.request_cancel(*first));
    CHECK(complete(queue, queued).state == JobState::cancelled); CHECK(calls == 0);
}
TEST_CASE("queue publication exception is fatal and close reclaims unacknowledged records")
{
    Environment env; JobQueue queue(env.queue_heap);
    auto id = queue.submit([](std::stop_token) -> Result<JobQueue::Payload> { return payload(); }, 1); REQUIRE(id);
    CHECK_THROWS_AS(complete(queue, *id, [](JobId, const auto&) -> Result<std::string> { throw std::runtime_error("publish infrastructure"); }), std::runtime_error);
    CHECK_THROWS_AS(queue.rethrow_failure(), std::runtime_error); queue.close();
    CHECK(queue.query(*id)->state == JobState::cancelled);
}
