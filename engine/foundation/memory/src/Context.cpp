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
ThreadContext::ThreadContext(MemorySystem& system, ResourceHandle upstream, ScratchOptions options)
    : ThreadContext(system)
{
    if (!upstream) { throw ContextError{ContextErrorCode::invalid_resource}; }
    if (system_id() != upstream.system_id()) { throw ContextError{ContextErrorCode::wrong_system}; }
    if (upstream.state() != ResourceState::open) { throw ContextError{ContextErrorCode::closing}; }
    scratch_ = std::make_unique<ScratchArena>(std::move(upstream), options);
}
ScratchArena& ThreadContext::scratch()
{
    if (thread_ != std::this_thread::get_id()) { throw ContextError{ContextErrorCode::wrong_thread}; }
    if (state() != ResourceState::open) { throw ContextError{ContextErrorCode::closing}; }
    if (!scratch_) { throw ContextError{ContextErrorCode::missing_scratch}; }
    return *scratch_;
}
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
