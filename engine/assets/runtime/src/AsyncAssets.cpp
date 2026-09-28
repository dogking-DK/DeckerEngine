#include <dk/assets/AsyncAssets.hpp>
#include <dk/memory/SmartPtr.hpp>
#include "AssetInternal.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <limits>

namespace dk {
struct AsyncAssets::Impl {
    struct Slot { std::string source; AssetStatus status; };
    struct Pending { JobId job; std::string source; std::uint64_t session, generation; AssetId requested; };
    ProjectPaths paths;
    JobQueue& queue;
    Prepare prepare;
    Vector<Slot> slots;
    Vector<Pending> pending;
    std::uint64_t session = 1;
    Impl(ProjectPaths p, JobQueue& q, Prepare fn) : paths(std::move(p)), queue(q), prepare(std::move(fn))
    { slots.reserve(1024); pending.reserve(q.limits().active); }
    Slot* find(std::string_view source) {
        const auto it = std::find_if(slots.begin(), slots.end(), [&](const Slot& s) { return s.source == source; });
        return it == slots.end() ? nullptr : &*it;
    }
    Result<JobId> submit(std::string_view source, std::optional<double> scale, AssetId requested) {
        using namespace asset_detail;
        return attempt<JobId>("submit asset", [&] {
            (void)relative_path(source);
            require(!scale || (std::isfinite(*scale) && *scale > 0), "unit_scale must be positive");
            auto* slot = find(source);
            require(slot || slots.size() < 1024, "Asset slot capacity exhausted", ErrorCode::invalid_state);
            const auto generation = slot ? slot->status.generation : 0;
            require(generation != UINT64_MAX, "Asset generation exhausted", ErrorCode::invalid_state);
            require(pending.size() < queue.limits().active, "Asset pending capacity exhausted", ErrorCode::invalid_state);
            // Allocate control metadata before publishing a queryable JobId.
            Slot next{std::string{source}, {requested, AssetState::loading, generation + 1, 0, {}, {}, {}}};
            Pending ticket{{}, std::string{source}, session, generation + 1, requested};
            const auto id = take(queue.submit([p = paths, source = std::string{source}, scale, fn = prepare](std::stop_token stop) -> Result<JobQueue::Payload> {
                auto value = fn(p, {source, scale}, stop);
                if (!value) return std::unexpected(value.error());
                return memory::make_shared<PreparedCachedAsset>(std::move(*value));
            }, 128 * 1024 * 1024));
            ticket.job = id; next.status.job = id;
            if (slot) {
                if (slot->status.job) (void)queue.request_cancel(*slot->status.job);
                *slot = std::move(next);
            } else slots.push_back(std::move(next));
            pending.push_back(std::move(ticket));
            return id;
        });
    }
};
AsyncAssets::AsyncAssets(ProjectPaths paths, JobQueue& queue, Prepare prepare)
    : impl_(std::make_unique<Impl>(std::move(paths), queue, std::move(prepare))) {}
AsyncAssets::~AsyncAssets()
{ for (const auto& ticket : impl_->pending) (void)impl_->queue.request_cancel(ticket.job); }
Result<JobId> AsyncAssets::import(std::string_view source, std::optional<double> scale)
{ return impl_->submit(source, scale, {}); }
AssetStatus AsyncAssets::status(AssetId id, std::string_view source) const
{
    auto* slot = impl_->find(source);
    AssetStatus result = slot ? slot->status : AssetStatus{}; result.id = id; return result;
}
Result<AssetStatus> AsyncAssets::load(AssetId id, std::string_view source)
{
    if (id.is_nil()) return std::unexpected(Error{ErrorCode::invalid_argument, "Nil AssetId"});
    const auto current = status(id, source);
    if (current.state == AssetState::ready || current.state == AssetState::loading) return current;
    auto job = impl_->submit(source, {}, id); if (!job) return std::unexpected(job.error());
    return status(id, source);
}
Result<AssetStatus> AsyncAssets::unload(AssetId id, std::string_view source)
{
    auto* slot = impl_->find(source); if (!slot) return status(id, source);
    if (slot->status.generation == UINT64_MAX) return std::unexpected(Error{ErrorCode::invalid_state, "Asset generation exhausted"});
    if (slot->status.job) (void)impl_->queue.request_cancel(*slot->status.job);
    const auto generation = slot->status.generation + 1;
    slot->status = {id, AssetState::unloaded, generation, 0, {}, {}, {}};
    return slot->status;
}
Result<void> AsyncAssets::reset_session()
{
    if (impl_->session == UINT64_MAX) return std::unexpected(Error{ErrorCode::invalid_state, "Asset session exhausted"});
    for (const auto& ticket : impl_->pending) (void)impl_->queue.request_cancel(ticket.job);
    ++impl_->session; impl_->slots.clear(); return {};
}
void AsyncAssets::pump()
{
    auto& s = *impl_;
    s.queue.drain([&](JobId job, const JobQueue::Payload& value) -> Result<std::string> {
        const auto ticket = std::find_if(s.pending.begin(), s.pending.end(), [&](const auto& p) { return p.job == job; });
        if (ticket == s.pending.end()) return std::unexpected(Error{ErrorCode::invalid_state, "Unknown asset completion"});
        auto* slot = s.find(ticket->source);
        if (!slot || ticket->session != s.session || slot->status.generation != ticket->generation)
            return std::unexpected(Error{ErrorCode::conflict, "Stale asset completion"});
        const auto candidate = std::static_pointer_cast<const PreparedCachedAsset>(value);
        const auto& outputs = candidate->value().artifact.data.outputs;
        if (!ticket->requested.is_nil() && std::none_of(outputs.begin(), outputs.end(), [&](const auto& o) { return o.id == ticket->requested; }))
            return std::unexpected(Error{ErrorCode::conflict, "Requested identity absent from candidate"});
        const auto& prepared = candidate->value();
        const auto summary = nlohmann::json{{"root_id", prepared.artifact.data.mesh.id.to_string()},
            {"key", std::string{prepared.key}}, {"cache_hit", prepared.cache_hit}}.dump();
        // Preallocate the shared owning block before committing any persistent state.
        auto ready = memory::make_shared<CachedAsset>();
        auto committed = candidate->publish(); if (!committed) return std::unexpected(committed.error());
        *ready = std::move(*committed);
        slot->status.data = std::move(ready); slot->status.state = AssetState::ready;
        slot->status.ready_generation = ticket->generation;
        return summary;
    });
    for (auto it = s.pending.begin(); it != s.pending.end();) {
        const auto job = s.queue.query(it->job);
        if (job && !terminal(job->state)) { ++it; continue; }
        auto* slot = s.find(it->source);
        if (slot && it->session == s.session && slot->status.generation == it->generation) {
            if (!job || job->state == JobState::cancelled) slot->status.state = AssetState::unloaded;
            else if (job->state == JobState::failed) { slot->status.state = AssetState::failed; slot->status.error = job->error; }
            slot->status.job.reset();
        }
        it = s.pending.erase(it);
    }
}
} // namespace dk
