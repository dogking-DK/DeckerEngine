#pragma once
#include <dk/graphics/ResourceValidation.hpp>
#include <variant>
#include <optional>

namespace dk::graphics::graph {
namespace detail { struct GraphState; struct HandleAccess; struct PlanState; struct ExecutionAccess; }
template<class Tag> class Id final {
public:
    Id() = default;
    // Liveness only; Graph operations also check identity and bounds.
    [[nodiscard]] explicit operator bool() const noexcept { return !owner_.expired(); }
    [[nodiscard]] bool operator==(const Id& other) const noexcept {
        return index_ == other.index_ && !owner_.owner_before(other.owner_) && !other.owner_.owner_before(owner_);
    }
private:
    friend struct detail::HandleAccess;
    std::weak_ptr<detail::GraphState> owner_;
    std::size_t index_ = 0;
};
using BufferId = Id<struct BufferTag>;
using ImageId = Id<struct ImageTag>;
using PassId = Id<struct PassTag>;
using ResourceId = std::variant<BufferId, ImageId>;
enum class Lifetime { transient, external };
struct Use {
    ResourceId resource;
    AccessDescription access;
};
struct PassDesc {
    std::string_view name;
    std::span<const Use> uses;
    bool side_effect = false;
};
struct BufferInfo {
    std::string_view name;
    BufferDesc description;
    Lifetime lifetime;
    bool initialized;
};
struct ImageInfo {
    std::string_view name;
    ImageDesc description;
    Lifetime lifetime;
    bool initialized;
};
struct Counts {
    std::size_t buffers = 0, images = 0, passes = 0, dependencies = 0, outputs = 0;
    bool operator==(const Counts&) const = default;
};
// Indices below belong to one compiled snapshot, never to another Graph or plan.
struct PlannedUse {
    std::size_t resource;
    AccessDescription access;
};
struct PlannedPass {
    std::string_view name;
    std::span<const PlannedUse> uses;
    bool side_effect = false, retained = false;
    std::optional<std::size_t> order_index;
};
struct PlannedResource {
    std::string_view name;
    std::variant<BufferDesc, ImageDesc> description;
    std::size_t declaration_index = 0; // Index within the source buffer or image declarations.
    Lifetime lifetime = Lifetime::transient;
    bool initialized = false, output = false, retained = false;
    std::optional<std::size_t> first_use, last_use, allocation_index;
};
enum class DependencyKind { explicit_order, read_after_write, write_after_read, write_after_write, layout_transition, contents };
struct Dependency {
    std::size_t before, after; // Pass declaration indices (indices into passes()).
    DependencyKind kind;
    std::optional<std::size_t> resource;
    bool operator==(const Dependency&) const = default;
};
struct TransientAllocation {
    std::size_t resource, create_before; // create_before is an index into order().
    // Last use in order(); absent for outputs retained by the result owner.
    // A logical interval only: physical destruction must still wait for GPU completion.
    std::optional<std::size_t> release_after;
    bool operator==(const TransientAllocation&) const = default;
};
// Independent immutable CPU plan. Views remain valid until the owner is replaced/destroyed.
class CompiledGraph final {
public:
    CompiledGraph() = default;
    CompiledGraph(CompiledGraph&&) noexcept = default;
    CompiledGraph& operator=(CompiledGraph&&) noexcept = default;
    CompiledGraph(const CompiledGraph&) = delete;
    CompiledGraph& operator=(const CompiledGraph&) = delete;
    [[nodiscard]] explicit operator bool() const noexcept { return bool(state_); }
    [[nodiscard]] std::span<const PlannedPass> passes() const noexcept;
    [[nodiscard]] std::span<const PlannedResource> resources() const noexcept;
    [[nodiscard]] std::span<const std::size_t> order() const noexcept;
    [[nodiscard]] std::span<const Dependency> dependencies() const noexcept;
    [[nodiscard]] std::span<const TransientAllocation> allocations() const noexcept;
private:
    friend class Graph;
    friend struct detail::ExecutionAccess;
    explicit CompiledGraph(std::shared_ptr<detail::PlanState> state) : state_(std::move(state)) {}
    std::shared_ptr<detail::PlanState> state_;
};
// CPU declarations and compilation only. All operations require external serialization.
// Borrowed info/span values expire on any mutation, reset, move assignment or destruction.
class Graph final {
public:
    Graph() = default;
    Graph(Graph&&) noexcept = default;
    Graph& operator=(Graph&&) noexcept = default;
    Graph(const Graph&) = delete;
    Graph& operator=(const Graph&) = delete;
    [[nodiscard]] static Result<Graph> create(memory::ResourceHandle resource);
    [[nodiscard]] Result<BufferId> declare_buffer(std::string_view name, const BufferDesc&,
        Lifetime lifetime = Lifetime::transient, bool initialized = false);
    [[nodiscard]] Result<ImageId> declare_image(std::string_view name, const ImageDesc&,
        Lifetime lifetime = Lifetime::transient, bool initialized = false);
    [[nodiscard]] Result<PassId> add_pass(const PassDesc&);
    [[nodiscard]] Result<void> add_dependency(PassId before, PassId after);
    [[nodiscard]] Result<void> remove_dependency(PassId before, PassId after);
    [[nodiscard]] Result<void> mark_output(ResourceId resource);
    [[nodiscard]] Result<BufferInfo> buffer(BufferId) const;
    [[nodiscard]] Result<ImageInfo> image(ImageId) const;
    [[nodiscard]] Result<PassDesc> pass(PassId) const;
    [[nodiscard]] Counts counts() const noexcept;
    [[nodiscard]] Result<void> validate() const;
    [[nodiscard]] Result<CompiledGraph> compile() const;
    // Replaces graph identity only after allocating an empty candidate successfully.
    [[nodiscard]] Result<void> reset();
private:
    explicit Graph(std::shared_ptr<detail::GraphState> state) : state_(std::move(state)) {}
    std::shared_ptr<detail::GraphState> state_;
};
} // namespace dk::graphics::graph
