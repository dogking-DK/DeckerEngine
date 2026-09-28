#include <dk/jobs/JobQueue.hpp>
#include <dk/memory/Containers.hpp>
#include <dk/memory/SmartPtr.hpp>
#include <dk/profiling/Profiler.hpp>
#include <algorithm>
#include <condition_variable>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace dk {
namespace {
Error missing() { return {ErrorCode::not_found, "Job is unknown or no longer retained"}; }
std::string clipped(std::string_view text, std::size_t limit)
{
    auto n = std::min(text.size(),limit);
    // Preserve an existing UTF-8 sequence at the truncation boundary.
    while (n && n < text.size() && (static_cast<unsigned char>(text[n]) & 0xc0U) == 0x80U) --n;
    return std::string{text.substr(0,n)};
}
void bound(Error& error)
{
    auto message = clipped(error.message,4096);
    std::vector<std::string> context;
    context.reserve(std::min(error.context.size(), std::size_t{8}));
    for (std::size_t i = 0; i < error.context.size() && i < 8; ++i)
        context.push_back(clipped(error.context[i],512));
    error.message.swap(message); error.context.swap(context); // Release excess capacity, not only logical size.
}
}
struct JobQueue::Impl {
    struct Record {
        JobSnapshot view;
        Work work;
        memory::RoutingToken token;
        std::stop_source stop;
        std::optional<Result<Payload>> completion;
        std::size_t bytes = 0;
        std::uint64_t finished = 0;
    };
    memory::ResourceHandle resource;
    JobLimits limits;
    std::function<void()> wake;
    std::thread::id owner = std::this_thread::get_id();
    mutable std::mutex mutex;
    mutable std::condition_variable changed;
    Vector<std::shared_ptr<Record>> records;
    Vector<std::shared_ptr<Record>> pending;
    std::size_t active = 0, bytes = 0;
    std::uint64_t sequence = 0, finished = 0;
    bool closing = false, consuming = false;
    std::exception_ptr fatal;
    std::jthread worker;

    Impl(memory::ResourceHandle r, JobLimits l, std::function<void()> w)
        : resource(r), limits(l), wake(std::move(w)), records(memory::Allocator<std::shared_ptr<Record>>{r}),
          pending(memory::Allocator<std::shared_ptr<Record>>{r})
    {
        if (!r || !l.queued || !l.active || !l.terminal || !l.input_bytes ||
            l.queued > 65536 || l.active > 65536 || l.terminal > 65536)
            throw std::invalid_argument("Invalid JobLimits/resource");
        records.reserve(l.active + l.terminal); pending.reserve(l.queued);
        worker = std::jthread([this] { run(); });
    }
    void check_owner() const
    {
        if (owner != std::this_thread::get_id() || consuming)
            throw std::logic_error("JobQueue mutation requires its non-reentrant owner thread");
    }
    std::shared_ptr<Record> find(JobId id) const
    {
        const auto it = std::find_if(records.begin(), records.end(), [&](const auto& r) { return r->view.id == id; });
        return it == records.end() ? nullptr : *it;
    }
    // Only metadata is retained after finish. Work/token/payload are released outside mutex.
    void finish(Record& r, JobState state)
    {
        r.view.state = state; r.finished = ++finished;
        --active; bytes -= r.bytes;
        std::size_t count = 0;
        for (const auto& item : records) if (terminal(item->view.state)) ++count;
        if (count > limits.terminal) {
            const auto oldest = std::min_element(records.begin(), records.end(), [](const auto& a, const auto& b) {
                const auto fa = terminal(a->view.state) ? a->finished : UINT64_MAX;
                const auto fb = terminal(b->view.state) ? b->finished : UINT64_MAX;
                return fa < fb;
            });
            records.erase(oldest);
        }
        ++sequence; changed.notify_all();
    }
    void signal() noexcept
    {
        changed.notify_all();
        if (wake) {
            try { wake(); } catch (...) {
                std::lock_guard lock(mutex); fatal = std::current_exception(); closing = true; ++sequence;
                changed.notify_all();
            }
        }
    }
    void run() noexcept
    {
        DK_PROFILE_THREAD_NAME("dk-cpu-worker");
        try {
            memory::ThreadContextCache contexts;
            for (;;) {
                std::shared_ptr<Record> record;
                Work work; memory::RoutingToken token;
                {
                    std::unique_lock lock(mutex);
                    changed.wait(lock, [&] { return closing || !pending.empty(); });
                    if (closing) break;
                    record = pending.front(); pending.erase(pending.begin());
                    record->view.state = JobState::running;
                    work = std::move(record->work); token = std::move(record->token);
                    ++sequence;
                }
                signal();
                std::optional<Result<Payload>> result;
                try {
                    DK_PROFILE_ZONE("Jobs.Execute"); DK_PROFILE_ZONE_TEXT(record->view.id.to_string());
                    if (!token.try_validate()) {
                        std::lock_guard lock(mutex); record->view.cancel_requested = true;
                    } else {
                        auto& context = contexts.acquire(token, {token.resource(), {}, token.resource(), {}});
                        memory::ExecutionScope execution(context, token); memory::ScratchScope scratch;
                        result.emplace(work(record->stop.get_token()));
                        if (!*result) bound(result->error());
                    }
                } catch (const memory::ContextError& error) {
                    if (error.code() != memory::ContextErrorCode::closing) throw;
                    std::lock_guard lock(mutex); record->view.cancel_requested = true;
                }
                work = {}; token = {};
                (void)contexts.try_clear();
                {
                    std::lock_guard lock(mutex);
                    if (!result) result.emplace(Payload{});
                    record->completion = std::move(result);
                    ++sequence;
                }
                signal();
            }
        } catch (...) {
            { std::lock_guard lock(mutex); fatal = std::current_exception(); closing = true; ++sequence; }
            signal();
        }
    }
};
JobQueue::JobQueue(memory::ResourceHandle resource, JobLimits limits, std::function<void()> wake)
    : impl_(std::make_unique<Impl>(resource, limits, std::move(wake))) {}
JobQueue::~JobQueue() { close(); }
JobLimits JobQueue::limits() const noexcept { return impl_->limits; }
Result<JobId> JobQueue::submit(Work work, std::size_t input_bytes)
{
    auto& s = *impl_; s.check_owner(); rethrow_failure();
    DK_PROFILE_ZONE("Jobs.Submit");
    if (!work || !input_bytes) return std::unexpected(Error{ErrorCode::invalid_argument, "Work and input byte reservation required"});
    auto token = memory::RoutingToken::capture();
    if (!token.try_validate()) return std::unexpected(Error{ErrorCode::invalid_state, "Submitter memory is closing"});
    std::lock_guard lock(s.mutex);
    if (s.closing) return std::unexpected(Error{ErrorCode::invalid_state, "JobQueue is closing"});
    if (s.pending.size() >= s.limits.queued || s.active >= s.limits.active || input_bytes > s.limits.input_bytes - s.bytes)
        return std::unexpected(Error{ErrorCode::invalid_state, "JobQueue capacity exhausted"});
    auto id = JobId::generate(); if (!id) return std::unexpected(id.error());
    auto record = memory::make_shared_in<Impl::Record>(s.resource);
    record->view.id = *id; record->work = std::move(work); record->token = std::move(token); record->bytes = input_bytes;
    s.records.push_back(record); s.pending.push_back(std::move(record));
    ++s.active; s.bytes += input_bytes; ++s.sequence; s.changed.notify_all();
    return *id;
}
Result<JobSnapshot> JobQueue::query(JobId id) const
{
    std::lock_guard lock(impl_->mutex);
    const auto r = impl_->find(id); if (!r) return std::unexpected(missing());
    return r->view;
}
Result<JobCancel> JobQueue::request_cancel(JobId id)
{
    auto& s = *impl_; s.check_owner();
    std::shared_ptr<Impl::Record> record; Work released; memory::RoutingToken token;
    JobCancel result;
    {
        std::lock_guard lock(s.mutex);
        record = s.find(id); if (!record) return std::unexpected(missing());
        result.accepted = !terminal(record->view.state);
        if (result.accepted) {
            record->view.cancel_requested = true;
            if (record->view.state == JobState::queued) {
                std::erase(s.pending, record); released = std::move(record->work); token = std::move(record->token);
                s.finish(*record, JobState::cancelled);
            }
            ++s.sequence;
        }
        result.job = record->view;
    }
    // stop_callback and captured destructors must never run under the queue lock.
    if (result.accepted) record->stop.request_stop();
    s.signal(); return result;
}
std::size_t JobQueue::drain(const Consumer& consumer)
{
    auto& s = *impl_; s.check_owner(); rethrow_failure();
    s.consuming = true;
    struct Reset { bool& b; ~Reset() { b = false; } } reset{s.consuming};
    std::size_t count = 0;
    try {
    for (;;) {
        std::shared_ptr<Impl::Record> record;
        std::optional<Result<Payload>> result;
        bool cancelled = false;
        {
            std::lock_guard lock(s.mutex);
            for (const auto& item : s.records) if (item->completion) { record = item; break; }
            if (!record) break;
            result = std::move(record->completion); record->completion.reset();
            cancelled = record->view.cancel_requested;
        }
        DK_PROFILE_ZONE("Jobs.Publish"); DK_PROFILE_ZONE_TEXT(record->view.id.to_string());
        Result<std::string> published = std::string{};
        if (!cancelled) {
            if (!*result) published = std::unexpected(std::move(result->error()));
            else if (consumer) {
                try { published = consumer(record->view.id, **result); }
                catch (...) {
                    std::lock_guard lock(s.mutex);
                    s.fatal = std::current_exception(); s.closing = true; ++s.sequence; s.changed.notify_all();
                    throw;
                }
            }
            else cancelled = true;
        }
        if (published) { auto summary = clipped(*published,4096); published->swap(summary); }
        else bound(published.error());
        // Release the large payload before exposing terminal state.
        result.reset();
        {
            std::lock_guard lock(s.mutex);
            if (!cancelled) {
                if (published) record->view.summary = std::move(*published);
                else record->view.error = std::move(published.error());
            }
            s.finish(*record, cancelled ? JobState::cancelled : published ? JobState::succeeded : JobState::failed);
        }
        ++count; s.signal();
    }
    } catch (...) {
        { std::lock_guard lock(s.mutex); s.fatal = std::current_exception(); s.closing = true; ++s.sequence; }
        s.signal(); throw;
    }
    return count;
}
Result<JobWait> JobQueue::wait(JobId id, Clock::time_point deadline) const
{
    auto& s = *impl_; std::unique_lock lock(s.mutex);
    auto r = s.find(id); if (!r) return std::unexpected(missing());
    s.changed.wait_until(lock, deadline, [&] { return terminal(r->view.state) || s.fatal; });
    return JobWait{r->view, !terminal(r->view.state)};
}
std::uint64_t JobQueue::change_sequence() const
{ std::lock_guard lock(impl_->mutex); return impl_->sequence; }
void JobQueue::wait_change(std::uint64_t sequence, Clock::time_point deadline) const
{
    std::unique_lock lock(impl_->mutex);
    impl_->changed.wait_until(lock, deadline, [&] { return impl_->sequence != sequence || impl_->fatal; });
}
void JobQueue::rethrow_failure() const
{
    std::exception_ptr failure;
    { std::lock_guard lock(impl_->mutex); failure = impl_->fatal; }
    if (failure) std::rethrow_exception(failure);
}
void JobQueue::close() noexcept
{
    auto& s = *impl_;
    if (s.owner != std::this_thread::get_id() || s.consuming) std::terminate();
    { std::lock_guard lock(s.mutex); s.closing = true; ++s.sequence; s.changed.notify_all(); }
    // Owner cannot mutate records concurrently; worker never inserts/erases except terminal eviction.
    // Take one shared reference at a time before calling user stop callbacks.
    for (std::size_t i = 0;; ++i) {
        std::shared_ptr<Impl::Record> record;
        { std::lock_guard lock(s.mutex);
          if (i >= s.records.size()) break;
          record = s.records[i]; if (!terminal(record->view.state)) record->view.cancel_requested = true; }
        record->stop.request_stop();
    }
    if (s.worker.joinable()) s.worker.join();
    for (auto& record : s.records) {
        record->work = {}; record->token = {}; record->completion.reset();
    }
    {
        std::lock_guard lock(s.mutex);
        while (s.active) {
            const auto it = std::find_if(s.records.begin(), s.records.end(), [](const auto& r) { return !terminal(r->view.state); });
            const auto record = *it;
            s.finish(*record, JobState::cancelled);
        }
        s.pending.clear(); ++s.sequence;
    }
    s.signal();
}
} // namespace dk
