#pragma once
#include <dk/core/StableId.hpp>
#include <dk/memory/Context.hpp>
#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>

namespace dk {
struct JobIdTag;
using JobId = StableId<JobIdTag>;
enum class JobState { queued, running, succeeded, failed, cancelled };
[[nodiscard]] constexpr bool terminal(JobState state) noexcept
{ return state != JobState::queued && state != JobState::running; }
struct JobLimits {
    std::size_t queued = 16, active = 17, terminal = 256;
    std::size_t input_bytes = 512 * 1024 * 1024;
};
struct JobProgress {
    std::string phase, unit;
    std::uint64_t completed = 0, submitted = 0, total = 0;
};
struct JobSnapshot {
    JobId id;
    JobState state = JobState::queued;
    bool cancel_requested = false;
    std::string summary;
    std::optional<Error> error;
    std::optional<JobProgress> progress;
};
struct JobWait { JobSnapshot job; bool timed_out = false; };
struct JobCancel { JobSnapshot job; bool accepted = false; };

// Owner-thread mutations; query/wait/change waits are thread safe. Never borrow
// submitter TLS/scratch in Work. Consumers must not reenter mutation methods.
class JobQueue final {
public:
    using Payload = std::shared_ptr<const void>;
    using Work = std::function<Result<Payload>(std::stop_token)>;
    // Reporter is borrowed for the duration of Work; invoke synchronously, never retain it.
    using Reporter = std::function<void(JobProgress)>;
    using ProgressWork = std::function<Result<Payload>(std::stop_token, const Reporter&)>;
    using Consumer = std::function<Result<std::string>(JobId, const Payload&)>;
    using Clock = std::chrono::steady_clock;
    explicit JobQueue(memory::ResourceHandle resource, JobLimits limits = {},
                      std::function<void()> wake = {});
    ~JobQueue();
    JobQueue(const JobQueue&) = delete;
    JobQueue& operator=(const JobQueue&) = delete;
    [[nodiscard]] Result<JobId> submit(Work work, std::size_t input_bytes);
    [[nodiscard]] Result<JobId> submit(ProgressWork work, std::size_t input_bytes, JobProgress initial);
    [[nodiscard]] Result<JobCancel> request_cancel(JobId id);
    [[nodiscard]] Result<JobSnapshot> query(JobId id) const;
    [[nodiscard]] Result<JobWait> wait(JobId id, Clock::time_point deadline) const;
    [[nodiscard]] std::uint64_t change_sequence() const;
    void wait_change(std::uint64_t sequence, Clock::time_point deadline) const;
    std::size_t drain(const Consumer& consumer);
    void rethrow_failure() const;
    void close() noexcept;
    [[nodiscard]] JobLimits limits() const noexcept;
private:
    [[nodiscard]] Result<JobId> submit_work(ProgressWork, std::size_t, std::optional<JobProgress>);
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace dk
