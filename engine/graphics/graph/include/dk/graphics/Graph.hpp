#pragma once
#include <dk/graphics/ResourceValidation.hpp>
#include <variant>

namespace dk::graphics::graph {
namespace detail { struct GraphState; struct HandleAccess; }
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
// CPU declarations only. All operations require external serialization.
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
    // Replaces graph identity only after allocating an empty candidate successfully.
    [[nodiscard]] Result<void> reset();
private:
    explicit Graph(std::shared_ptr<detail::GraphState> state) : state_(std::move(state)) {}
    std::shared_ptr<detail::GraphState> state_;
};
} // namespace dk::graphics::graph
