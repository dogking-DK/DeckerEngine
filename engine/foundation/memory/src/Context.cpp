#include <dk/memory/Context.hpp>
#include <utility>

namespace dk::memory {
namespace {
thread_local detail::RoutingFrame* current = nullptr;
ThreadContext& current_context()
{
    if (!current) { throw ContextError{ContextErrorCode::missing_context}; }
    return current->context;
}
ResourceHandle validated_resource(const RoutingToken& token)
{
    const auto valid = token.try_validate();
    if (!valid) { throw ContextError{valid.error()}; }
    return token.resource();
}
bool same_options(const ThreadContextOptions& a, const ThreadContextOptions& b) noexcept
{
    return a.scratch_upstream == b.scratch_upstream && a.local_pool_upstream == b.local_pool_upstream
        && a.scratch_options.chunk_bytes == b.scratch_options.chunk_bytes
        && a.scratch_options.max_retained_bytes == b.scratch_options.max_retained_bytes
        && a.local_pool_options.max_blocks_per_chunk == b.local_pool_options.max_blocks_per_chunk
        && a.local_pool_options.largest_required_pool_block == b.local_pool_options.largest_required_pool_block;
}
}
const char* ContextError::what() const noexcept
{
    switch (code_) {
    case ContextErrorCode::missing_context: return "No memory execution scope is bound";
    case ContextErrorCode::invalid_resource: return "Invalid memory resource";
    case ContextErrorCode::wrong_system: return "Memory resource belongs to another system";
    case ContextErrorCode::wrong_thread: return "Memory context belongs to another thread";
    case ContextErrorCode::closing: return "Memory system or resource is closing";
    case ContextErrorCode::missing_scratch: return "Thread context has no scratch arena";
    case ContextErrorCode::missing_scope: return "No scratch scope is bound";
    case ContextErrorCode::missing_pool: return "Thread context has no local pool";
    case ContextErrorCode::invalid_token: return "Invalid memory routing token";
    case ContextErrorCode::configuration_mismatch: return "Cached thread context has a different configuration";
    }
    return "Memory context error";
}
detail::RoutingFrame::RoutingFrame(ThreadContext& owner, ResourceHandle selected)
    : context(owner), resource(std::move(selected))
{
    if (context.thread_ != std::this_thread::get_id()) { throw ContextError{ContextErrorCode::wrong_thread}; }
    if (!resource) { throw ContextError{ContextErrorCode::invalid_resource}; }
    if (context.system_id() != resource.system_id()) { throw ContextError{ContextErrorCode::wrong_system}; }
    if (context.state() != ResourceState::open || resource.state() != ResourceState::open) {
        throw ContextError{ContextErrorCode::closing};
    }
    previous = current;
    ++context.scopes_;
    current = this; // Publish only after every validation succeeds.
}
detail::RoutingFrame::~RoutingFrame()
{
    if (context.thread_ != std::this_thread::get_id() || current != this || scratch != inherited_scratch) { std::terminate(); }
    current = previous;
    --context.scopes_;
}
ExecutionScope::ExecutionScope(ThreadContext& context, ResourceHandle resource)
    : frame_(context, std::move(resource)) {}
ExecutionScope::ExecutionScope(ThreadContext& context, const RoutingToken& token)
    : frame_(context, validated_resource(token)) {}
DomainScope::DomainScope(ResourceHandle resource) : frame_(current_context(), std::move(resource))
{
    frame_.scratch = frame_.previous->scratch;
    frame_.inherited_scratch = frame_.scratch;
}
std::expected<ResourceHandle, ContextErrorCode> try_current_resource() noexcept
{
    if (!current) { return std::unexpected(ContextErrorCode::missing_context); }
    if (current->context.state() != ResourceState::open || current->resource.state() != ResourceState::open) {
        return std::unexpected(ContextErrorCode::closing);
    }
    return current->resource;
}
ResourceHandle current_resource()
{
    auto result = try_current_resource();
    if (!result) { throw ContextError{result.error()}; }
    return std::move(*result);
}
RoutingToken RoutingToken::capture() { return from_resource(current_resource()); }
ThreadContext::ThreadContext(MemorySystem& system, ResourceHandle upstream, ScratchOptions options)
    : ThreadContext(system)
{
    if (!upstream) { throw ContextError{ContextErrorCode::invalid_resource}; }
    if (system_id() != upstream.system_id()) { throw ContextError{ContextErrorCode::wrong_system}; }
    if (upstream.state() != ResourceState::open) { throw ContextError{ContextErrorCode::closing}; }
    configure({std::move(upstream), options, {}, {}});
}
ThreadContext::ThreadContext(MemorySystem& system, ThreadContextOptions options) : ThreadContext(system)
{ configure(std::move(options)); }
ThreadContext::ThreadContext(const RoutingToken& token, ThreadContextOptions options) : ThreadContext(token.system_)
{
    (void)validated_resource(token);
    configure(std::move(options));
}
void ThreadContext::configure(ThreadContextOptions options)
{
    for (const auto& upstream : {options.scratch_upstream, options.local_pool_upstream}) {
        if (!upstream) { continue; }
        if (system_id() != upstream.system_id()) { throw ContextError{ContextErrorCode::wrong_system}; }
        if (upstream.state() != ResourceState::open) { throw ContextError{ContextErrorCode::closing}; }
    }
    if (options.scratch_upstream) { scratch_ = std::make_unique<ScratchArena>(options.scratch_upstream, options.scratch_options); }
    if (options.local_pool_upstream) { local_pool_ = std::make_unique<LocalPoolResource>(options.local_pool_upstream, options.local_pool_options); }
    options_ = std::move(options);
}
ScratchArena& ThreadContext::scratch()
{
    if (thread_ != std::this_thread::get_id()) { throw ContextError{ContextErrorCode::wrong_thread}; }
    if (state() != ResourceState::open) { throw ContextError{ContextErrorCode::closing}; }
    if (!scratch_) { throw ContextError{ContextErrorCode::missing_scratch}; }
    return *scratch_;
}
LocalPoolResource& ThreadContext::local_pool()
{
    if (thread_ != std::this_thread::get_id()) { throw ContextError{ContextErrorCode::wrong_thread}; }
    if (state() != ResourceState::open) { throw ContextError{ContextErrorCode::closing}; }
    if (!local_pool_) { throw ContextError{ContextErrorCode::missing_pool}; }
    return *local_pool_;
}
LocalPoolResource& current_local_pool() { return current_context().local_pool(); }
bool ThreadContext::idle() const
{
    if (scopes_ || (scratch_ && scratch_->snapshot().active_scopes)) { return false; }
    if (local_pool_) {
        const auto snapshot = local_pool_->snapshot();
        if (snapshot.live_allocations || snapshot.active_operations) { return false; }
    }
    return true;
}
ThreadContextCache::~ThreadContextCache()
{
    if (thread_ != std::this_thread::get_id()) { std::terminate(); }
    // Each ThreadContext enforces that every borrowed scope/object has ended.
}
std::size_t ThreadContextCache::size() const
{
    if (thread_ != std::this_thread::get_id()) { throw ContextError{ContextErrorCode::wrong_thread}; }
    return contexts_.size();
}
ThreadContext& ThreadContextCache::acquire(const RoutingToken& token, ThreadContextOptions options)
{
    if (thread_ != std::this_thread::get_id()) { throw ContextError{ContextErrorCode::wrong_thread}; }
    (void)validated_resource(token);
    for (const auto& context : contexts_) {
        if (context->system_id() != token.system_id()) { continue; }
        if (!same_options(context->options_, options)) { throw ContextError{ContextErrorCode::configuration_mismatch}; }
        for (const auto& upstream : {options.scratch_upstream, options.local_pool_upstream}) {
            if (upstream && upstream.state() != ResourceState::open) { throw ContextError{ContextErrorCode::closing}; }
        }
        return *context;
    }
    auto candidate = std::unique_ptr<ThreadContext>{new ThreadContext{token, std::move(options)}};
    auto& result = *candidate;
    contexts_.push_back(std::move(candidate)); // Only publish a fully configured context.
    return result;
}
std::expected<ContextRetirement, ContextErrorCode> ThreadContextCache::retire(bool all) noexcept
{
    if (thread_ != std::this_thread::get_id()) { return std::unexpected(ContextErrorCode::wrong_thread); }
    ContextRetirement result;
    for (auto it = contexts_.begin(); it != contexts_.end();) {
        if (!all && (*it)->state() == ResourceState::open) { ++it; continue; }
        if (!(*it)->idle()) { ++result.busy; ++it; continue; }
        it = contexts_.erase(it);
        ++result.retired;
    }
    return result;
}
std::expected<ContextRetirement, ContextErrorCode> ThreadContextCache::try_retire_closed() noexcept { return retire(false); }
std::expected<ContextRetirement, ContextErrorCode> ThreadContextCache::try_clear() noexcept { return retire(true); }
ScratchScope::ScratchScope(ScratchArena& arena) : arena_(&arena)
{
    auto result = arena.try_checkpoint();
    if (!result) { throw ScratchError{result.error()}; }
    checkpoint_ = *result;
}
ScratchScope::ScratchScope() : ScratchScope(current_context().scratch())
{
    frame_ = current;
    previous_ = frame_->scratch;
    frame_->scratch = this;
}
ScratchScope::~ScratchScope()
{
    if (frame_ && (current != frame_ || frame_->scratch != this)) { std::terminate(); }
    if (!arena_->try_rewind(checkpoint_)) { std::terminate(); }
    if (frame_) { frame_->scratch = previous_; }
}
std::pmr::memory_resource* current_scratch_resource()
{
    if (!current) { throw ContextError{ContextErrorCode::missing_context}; }
    if (current->context.state() != ResourceState::open) { throw ContextError{ContextErrorCode::closing}; }
    if (!current->scratch) { throw ContextError{ContextErrorCode::missing_scope}; }
    return current->scratch->resource();
}
} // namespace dk::memory
