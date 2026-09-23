#pragma once
#include <dk/memory/Arena.hpp>
#include <exception>
#include <thread>

namespace dk::memory {
class MemorySystem;
namespace detail { struct SystemState; struct RoutingFrame; }

enum class ContextErrorCode { missing_context, invalid_resource, wrong_system, wrong_thread, closing, missing_scratch, missing_scope };
class ContextError final : public std::exception {
public:
    explicit ContextError(ContextErrorCode code) noexcept : code_(code) {}
    [[nodiscard]] ContextErrorCode code() const noexcept { return code_; }
    [[nodiscard]] const char* what() const noexcept override;
private:
    ContextErrorCode code_;
};

// Stack-owned, thread-affine lease. Must outlive every scope using it.
class ThreadContext {
public:
    explicit ThreadContext(MemorySystem& system);
    ThreadContext(MemorySystem& system, ResourceHandle scratch_upstream, ScratchOptions options = {});
    ~ThreadContext();
    ThreadContext(const ThreadContext&) = delete;
    ThreadContext& operator=(const ThreadContext&) = delete;
    ThreadContext(ThreadContext&&) = delete;
    ThreadContext& operator=(ThreadContext&&) = delete;
    [[nodiscard]] SystemId system_id() const noexcept;
    [[nodiscard]] ResourceState state() const noexcept;
    [[nodiscard]] ScratchArena& scratch();
private:
    friend struct detail::RoutingFrame;
    std::shared_ptr<detail::SystemState> system_;
    std::thread::id thread_;
    std::size_t scopes_ = 0; // Only accessed by the owner thread.
    std::unique_ptr<ScratchArena> scratch_;
};

namespace detail {
struct RoutingFrame {
    RoutingFrame(ThreadContext& context, ResourceHandle resource);
    ~RoutingFrame();
    RoutingFrame(const RoutingFrame&) = delete;
    RoutingFrame& operator=(const RoutingFrame&) = delete;
    ThreadContext& context;
    ResourceHandle resource;
    RoutingFrame* previous = nullptr;
    ScratchScope* scratch = nullptr;
    ScratchScope* inherited_scratch = nullptr;
};
}

// Neither scope is copyable/movable. Destruction must be on its owner thread, LIFO.
class ExecutionScope {
public:
    ExecutionScope(ThreadContext& context, ResourceHandle resource);
private:
    detail::RoutingFrame frame_;
};
class DomainScope {
public:
    explicit DomainScope(ResourceHandle resource);
private:
    detail::RoutingFrame frame_;
};

[[nodiscard]] std::expected<ResourceHandle, ContextErrorCode> try_current_resource() noexcept;
[[nodiscard]] ResourceHandle current_resource();
} // namespace dk::memory
