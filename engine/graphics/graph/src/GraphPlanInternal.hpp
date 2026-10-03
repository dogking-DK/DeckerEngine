#pragma once
#include "GraphInternal.hpp"

namespace dk::graphics::graph {
namespace detail {
struct PlanResourceOwner {
    PlanResourceOwner(memory::ResourceHandle heap, std::string_view label) : name(label, memory::Allocator<char>{heap}) {}
    String name;
};
struct PlanPassOwner {
    PlanPassOwner(memory::ResourceHandle heap, std::string_view label)
        : name(label, memory::Allocator<char>{heap}), uses(0, memory::Allocator<PlannedUse>{heap}) {}
    String name;
    Vector<PlannedUse> uses;
};
struct PlanState {
    explicit PlanState(memory::ResourceHandle heap)
        : resource(heap), resource_owners(0, memory::Allocator<memory::UniquePtr<PlanResourceOwner>>{heap}),
          pass_owners(0, memory::Allocator<memory::UniquePtr<PlanPassOwner>>{heap}),
          resources(0, memory::Allocator<PlannedResource>{heap}), passes(0, memory::Allocator<PlannedPass>{heap}),
          order(0, memory::Allocator<std::size_t>{heap}), dependencies(0, memory::Allocator<Dependency>{heap}),
          allocations(0, memory::Allocator<TransientAllocation>{heap}) {}
    // Count constructors and pointer-owned records keep Debug proxy allocation failures throwable.
    memory::ResourceHandle resource;
    Vector<memory::UniquePtr<PlanResourceOwner>> resource_owners;
    Vector<memory::UniquePtr<PlanPassOwner>> pass_owners;
    Vector<PlannedResource> resources;
    Vector<PlannedPass> passes;
    Vector<std::size_t> order;
    Vector<Dependency> dependencies;
    Vector<TransientAllocation> allocations;
};
} // namespace detail
} // namespace dk::graphics::graph
