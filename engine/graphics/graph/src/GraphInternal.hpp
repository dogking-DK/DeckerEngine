#pragma once
#include <dk/graphics/Graph.hpp>
#include <dk/memory/Containers.hpp>
#include <dk/memory/SmartPtr.hpp>

namespace dk::graphics::graph {
namespace detail {
struct HandleAccess {
    template<class Tag> static Id<Tag> make(const std::shared_ptr<GraphState>& state, std::size_t index) {
        Id<Tag> id; id.owner_ = state; id.index_ = index; return id;
    }
    template<class Tag> static bool belongs(const Id<Tag>& id, const std::shared_ptr<GraphState>& state) {
        return id.owner_.lock() == state;
    }
    template<class Tag> static std::size_t index(const Id<Tag>& id) { return id.index_; }
};
template<class Desc> struct Resource {
    Resource(memory::ResourceHandle heap, std::string_view label, const Desc& desc, Lifetime life, bool contents)
        : name(label, memory::Allocator<char>{heap}), description(desc), lifetime(life), initialized(contents) {}
    String name;
    Desc description;
    Lifetime lifetime;
    bool initialized;
};
struct Pass {
    Pass(memory::ResourceHandle heap, const PassDesc& desc)
        : name(desc.name, memory::Allocator<char>{heap}), uses(0, memory::Allocator<Use>{heap}), side_effect(desc.side_effect) {}
    String name;
    Vector<Use> uses;
    bool side_effect;
};
struct Edge {
    std::size_t before, after;
    bool operator==(const Edge&) const = default;
};
struct GraphState {
    explicit GraphState(memory::ResourceHandle heap)
        : resource(heap), buffers(0, memory::Allocator<memory::UniquePtr<Resource<BufferDesc>>>{heap}),
          images(0, memory::Allocator<memory::UniquePtr<Resource<ImageDesc>>>{heap}),
          passes(0, memory::Allocator<memory::UniquePtr<Pass>>{heap}),
          dependencies(0, memory::Allocator<Edge>{heap}), outputs(0, memory::Allocator<ResourceId>{heap}) {}
    memory::ResourceHandle resource;
    Vector<memory::UniquePtr<Resource<BufferDesc>>> buffers;
    Vector<memory::UniquePtr<Resource<ImageDesc>>> images;
    Vector<memory::UniquePtr<Pass>> passes;
    Vector<Edge> dependencies;
    Vector<ResourceId> outputs;
};

// Shared declaration analysis. Populates ordering constraints only; no culling.
Result<void> analyze(const std::shared_ptr<GraphState>& state, Vector<Dependency>& dependencies);
void append_dependency(Vector<Dependency>& dependencies, const Dependency& dependency);
std::size_t resource_number(const ResourceId& id, const GraphState& state);
Use complete_use(const ResourceId& id, const GraphState& state);
} // namespace detail
} // namespace dk::graphics::graph
