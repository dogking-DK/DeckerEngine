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
    }
    return "Memory context error";
}
detail::RoutingFrame::RoutingFrame(ThreadContext& owner, ResourceHandle selected)
    : context(owner), resource(std::move(selected))
{
    if (context.thread_ != std::this_thread::get_id()) { throw ContextError{ContextErrorCode::wrong_thread}; }
    if (!resource) { throw ContextError{ContextErrorCode::invalid_resource}; }
    if (context.system_id() != resource.system_id()) { throw ContextError{ContextErrorCode::wrong_system}; }
    if (context.state() != ResourceState::open || resource.snapshot().state != ResourceState::open) {
        throw ContextError{ContextErrorCode::closing};
    }
    previous = current;
    ++context.scopes_;
    current = this; // Publish only after every validation succeeds.
}
detail::RoutingFrame::~RoutingFrame()
{
    if (context.thread_ != std::this_thread::get_id() || current != this) { std::terminate(); }
    current = previous;
    --context.scopes_;
}
ExecutionScope::ExecutionScope(ThreadContext& context, ResourceHandle resource)
    : frame_(context, std::move(resource)) {}
DomainScope::DomainScope(ResourceHandle resource) : frame_(current_context(), std::move(resource)) {}
std::expected<ResourceHandle, ContextErrorCode> try_current_resource() noexcept
{
    if (!current) { return std::unexpected(ContextErrorCode::missing_context); }
    if (current->context.state() != ResourceState::open || current->resource.snapshot().state != ResourceState::open) {
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
} // namespace dk::memory
