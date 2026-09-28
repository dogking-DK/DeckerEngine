#pragma once
#include <dk/memory/Arena.hpp>
#include <dk/memory/Pool.hpp>
#include <exception>
#include <thread>

namespace dk::memory {
class MemorySystem;
namespace detail { struct SystemState; struct RoutingFrame; }

enum class ContextErrorCode {
    missing_context, invalid_resource, wrong_system, wrong_thread, closing,
    missing_scratch, missing_scope, missing_pool, invalid_token, configuration_mismatch
};
class ContextError final : public std::exception {
public:
    explicit ContextError(ContextErrorCode code) noexcept : code_(code) {}
    [[nodiscard]] ContextErrorCode code() const noexcept { return code_; }
    [[nodiscard]] const char* what() const noexcept override;
private:
    ContextErrorCode code_;
};

// Owns only persistent routing state. Safe to copy to another thread; does not keep a system Open.
class RoutingToken {
public:
    RoutingToken() noexcept = default;
    [[nodiscard]] static RoutingToken capture();
    [[nodiscard]] static RoutingToken from_resource(ResourceHandle resource);
    [[nodiscard]] explicit operator bool() const noexcept { return bool(system_) && bool(resource_); }
    [[nodiscard]] SystemId system_id() const noexcept { return resource_.system_id(); }
    [[nodiscard]] ResourceHandle resource() const noexcept { return resource_; }
    [[nodiscard]] std::expected<void, ContextErrorCode> try_validate() const noexcept;
private:
    friend class ThreadContext;
    std::shared_ptr<detail::SystemState> system_;
    ResourceHandle resource_;
};

struct ThreadContextOptions {
    ResourceHandle scratch_upstream;
    ScratchOptions scratch_options;
    ResourceHandle local_pool_upstream;
    PoolOptions local_pool_options;
};

// Thread-affine lease. Must outlive every scope and local resource borrower using it.
class ThreadContext {
public:
    explicit ThreadContext(MemorySystem& system);
    ThreadContext(MemorySystem& system, ResourceHandle scratch_upstream, ScratchOptions options = {});
    ThreadContext(MemorySystem& system, ThreadContextOptions options);
    ~ThreadContext();
    ThreadContext(const ThreadContext&) = delete;
    ThreadContext& operator=(const ThreadContext&) = delete;
    ThreadContext(ThreadContext&&) = delete;
    ThreadContext& operator=(ThreadContext&&) = delete;
    [[nodiscard]] SystemId system_id() const noexcept;
    [[nodiscard]] ResourceState state() const noexcept;
    [[nodiscard]] ScratchArena& scratch();
    [[nodiscard]] LocalPoolResource& local_pool();
private:
    friend struct detail::RoutingFrame;
    friend class ThreadContextCache;
    explicit ThreadContext(std::shared_ptr<detail::SystemState> system);
    ThreadContext(const RoutingToken& token, ThreadContextOptions options);
    void configure(ThreadContextOptions options);
    [[nodiscard]] bool idle() const;
    std::shared_ptr<detail::SystemState> system_;
    std::thread::id thread_;
    std::size_t scopes_ = 0; // Only accessed by the owner thread.
    std::unique_ptr<ScratchArena> scratch_;
    std::unique_ptr<LocalPoolResource> local_pool_;
    ThreadContextOptions options_;
};

struct ContextRetirement {
    std::size_t retired = 0;
    std::size_t busy = 0;
};

// Explicit executor-owned cache, not a TLS owner. All methods/destruction are owner-thread only.
class ThreadContextCache {
public:
    ThreadContextCache() noexcept : thread_(std::this_thread::get_id()) {}
    ~ThreadContextCache();
    ThreadContextCache(const ThreadContextCache&) = delete;
    ThreadContextCache& operator=(const ThreadContextCache&) = delete;
    ThreadContextCache(ThreadContextCache&&) = delete;
    ThreadContextCache& operator=(ThreadContextCache&&) = delete;
    // Returned reference is borrowed until retirement/clear/destruction; configuration cannot change on reuse.
    [[nodiscard]] ThreadContext& acquire(const RoutingToken& token, ThreadContextOptions options = {});
    [[nodiscard]] std::size_t size() const;
    [[nodiscard]] std::expected<ContextRetirement, ContextErrorCode> try_retire_closed() noexcept;
    [[nodiscard]] std::expected<ContextRetirement, ContextErrorCode> try_clear() noexcept;
private:
    [[nodiscard]] std::expected<ContextRetirement, ContextErrorCode> retire(bool all) noexcept;
    std::thread::id thread_;
    std::vector<std::unique_ptr<ThreadContext>> contexts_;
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
    ExecutionScope(ThreadContext& context, const RoutingToken& token);
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
[[nodiscard]] LocalPoolResource& current_local_pool();
} // namespace dk::memory
