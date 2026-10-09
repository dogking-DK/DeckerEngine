#include <dk/profiling/Profiler.hpp>
#include "GraphInternal.hpp"
#include "GraphPlanInternal.hpp"
#include <algorithm>
#include <functional>
#include <tuple>

namespace dk::graphics::graph {

namespace {
using detail::GraphState;
using detail::PlanState;

void snapshot(const GraphState& graph, PlanState& plan) {
    const auto copy_resources = [&](const auto& declarations) {
        for (std::size_t i = 0; i < declarations.size(); ++i) {
            const auto& source = *declarations[i];
            auto owner = memory::make_unique_in<detail::PlanResourceOwner>(plan.resource, plan.resource, std::string_view{source.name});
            plan.resource_owners.push_back(std::move(owner));
            PlannedResource resource;
            resource.name = plan.resource_owners.back()->name;
            resource.description = source.description;
            resource.declaration_index = i;
            resource.lifetime = source.lifetime;
            resource.initialized = source.initialized;
            plan.resources.push_back(resource);
        }
    };
    copy_resources(graph.buffers);
    copy_resources(graph.images);
    for (const auto& source : graph.passes) {
        auto owner = memory::make_unique_in<detail::PlanPassOwner>(plan.resource, plan.resource, std::string_view{source->name});
        owner->uses.reserve(source->uses.size());
        for (const auto& use : source->uses) owner->uses.push_back({detail::resource_number(use.resource, graph), use.access});
        plan.pass_owners.push_back(std::move(owner));
        const auto& stored = *plan.pass_owners.back();
        plan.passes.push_back({stored.name, stored.uses, source->side_effect, false, {}});
    }
}

// Walk latest producers, subtracting only the range each writer contributes.
// A preserving write supplies its range by depending on its own previous contents.
template<class F> void latest_writers(const GraphState& graph, const Use& use, std::size_t before, F found) {
    struct Interval { vk::DeviceSize begin, end; };
    Vector<Interval> pending(0, memory::Allocator<Interval>{graph.resource});
    Vector<Interval> remainder(0, memory::Allocator<Interval>{graph.resource});
    const bool buffer = std::holds_alternative<BufferId>(use.resource);
    const auto& range = use.access.range;
    const auto first_mip = buffer ? 0u : range.baseMipLevel;
    const auto end_mip = buffer ? 1u : range.baseMipLevel + range.levelCount;
    for (auto mip = first_mip; mip < end_mip; ++mip) {
        pending.clear();
        const vk::DeviceSize begin = buffer ? use.access.offset : range.baseArrayLayer;
        pending.push_back({begin, begin + (buffer ? use.access.size : range.layerCount)});
        for (std::size_t p = before; p > 0 && !pending.empty(); --p) {
            for (const auto& write : graph.passes[p - 1]->uses) {
                if (write.resource != use.resource || !access_writes(write.access.state.access)) continue;
                const auto& written = write.access.range;
                if (!buffer && (mip < written.baseMipLevel || mip >= written.baseMipLevel + written.levelCount)) continue;
                const vk::DeviceSize start = buffer ? write.access.offset : written.baseArrayLayer;
                const vk::DeviceSize finish = start + (buffer ? write.access.size : written.layerCount);
                remainder.clear();
                bool contributes = false;
                for (const auto& part : pending) {
                    if (start >= part.end || finish <= part.begin) { remainder.push_back(part); continue; }
                    contributes = true;
                    if (part.begin < start) remainder.push_back({part.begin, start});
                    if (finish < part.end) remainder.push_back({finish, part.end});
                }
                pending.swap(remainder);
                if (contributes) found(p - 1);
            }
        }
    }
}

void cull(const GraphState& graph, PlanState& plan) {
    Vector<std::size_t> pending(0, memory::Allocator<std::size_t>{plan.resource});
    Vector<Dependency> required(0, memory::Allocator<Dependency>{plan.resource});
    for (const auto& edge : plan.dependencies) if (edge.kind == DependencyKind::explicit_order) required.push_back(edge);
    const auto keep = [&](std::size_t index) {
        if (plan.passes[index].retained) return;
        plan.passes[index].retained = true;
        pending.push_back(index);
    };
    for (std::size_t p = 0; p < graph.passes.size(); ++p) {
        if (graph.passes[p]->side_effect) keep(p);
        for (const auto& use : graph.passes[p]->uses) {
            if (use.access.full_overwrite) continue;
            latest_writers(graph, use, p, [&](std::size_t producer) {
                const Dependency edge{producer, p, DependencyKind::contents, detail::resource_number(use.resource, graph)};
                plan.dependencies.push_back(edge); required.push_back(edge);
            });
        }
    }
    for (const auto& output : graph.outputs) {
        auto& resource = plan.resources[detail::resource_number(output, graph)];
        resource.output = resource.retained = true;
        latest_writers(graph, detail::complete_use(output, graph), graph.passes.size(), keep);
    }
    std::ranges::sort(required, {}, &Dependency::after);
    while (!pending.empty()) {
        const auto consumer = pending.back(); pending.pop_back();
        for (auto edge = std::ranges::lower_bound(required, consumer, {}, &Dependency::after);
            edge != required.end() && edge->after == consumer; ++edge) keep(edge->before);
    }
    std::erase_if(plan.dependencies, [&](const Dependency& edge) {
        return !plan.passes[edge.before].retained || !plan.passes[edge.after].retained;
    });
    detail::normalize_dependencies(plan.dependencies);
}

Result<void> schedule(PlanState& plan) {
    Vector<std::size_t> indegree(plan.passes.size(), 0, memory::Allocator<std::size_t>{plan.resource});
    Vector<std::size_t> ready(0, memory::Allocator<std::size_t>{plan.resource});
    std::size_t retained = 0;
    for (const auto& edge : plan.dependencies) ++indegree[edge.after];
    for (std::size_t i = 0; i < plan.passes.size(); ++i) {
        if (!plan.passes[i].retained) continue;
        ++retained;
        if (!indegree[i]) ready.push_back(i);
    }
    std::ranges::make_heap(ready, std::greater<std::size_t>{});
    while (!ready.empty()) {
        std::ranges::pop_heap(ready, std::greater<std::size_t>{});
        const auto next = ready.back(); ready.pop_back();
        plan.passes[next].order_index = plan.order.size();
        plan.order.push_back(next);
        for (auto edge = std::ranges::lower_bound(plan.dependencies, next, {}, &Dependency::before);
            edge != plan.dependencies.end() && edge->before == next; ++edge) {
            if (--indegree[edge->after] == 0) {
                ready.push_back(edge->after);
                std::ranges::push_heap(ready, std::greater<std::size_t>{});
            }
        }
    }
    if (plan.order.size() != retained)
        return std::unexpected(Error{ErrorCode::internal_error, "compiled dependencies are not acyclic"});
    return {};
}

Result<void> lifetimes(PlanState& plan) {
    for (std::size_t position = 0; position < plan.order.size(); ++position) {
        for (const auto& use : plan.passes[plan.order[position]].uses) {
            auto& resource = plan.resources[use.resource];
            resource.retained = true;
            if (!resource.first_use) resource.first_use = position;
            resource.last_use = position;
        }
    }
    for (std::size_t i = 0; i < plan.resources.size(); ++i) {
        auto& resource = plan.resources[i];
        if (!resource.retained || resource.lifetime == Lifetime::external) continue;
        if (!resource.first_use || !resource.last_use)
            return std::unexpected(Error{ErrorCode::internal_error, "retained transient resource has no producer"});
        plan.allocations.push_back({i, *resource.first_use, resource.output ? std::nullopt : resource.last_use});
    }
    std::ranges::sort(plan.allocations, [](const TransientAllocation& a, const TransientAllocation& b) {
        return std::tie(a.create_before, a.resource) < std::tie(b.create_before, b.resource);
    });
    for (std::size_t i = 0; i < plan.allocations.size(); ++i) plan.resources[plan.allocations[i].resource].allocation_index = i;
    return {};
}
} // namespace

Result<CompiledGraph> Graph::compile() const try {
    DK_PROFILE_ZONE("graph.compile");
    if (!state_ || state_->resource.state() != memory::ResourceState::open)
        return std::unexpected(Error{ErrorCode::invalid_state, "graph is empty, moved from, or its memory domain is closing"});
    auto candidate = memory::make_shared_in<PlanState>(state_->resource, state_->resource);
    if (auto valid = detail::analyze(state_, candidate->dependencies); !valid) return std::unexpected(valid.error());
    snapshot(*state_, *candidate);
    cull(*state_, *candidate);
    if (auto sorted = schedule(*candidate); !sorted) return std::unexpected(sorted.error());
    if (auto planned = lifetimes(*candidate); !planned) return std::unexpected(planned.error());
    return CompiledGraph{std::move(candidate)};
} catch (const std::bad_alloc&) {
    return std::unexpected(Error{ErrorCode::internal_error, "graph compilation allocation failed; graph and prior plans unchanged"});
}
std::span<const PlannedPass> CompiledGraph::passes() const noexcept { return state_ ? std::span<const PlannedPass>{state_->passes} : std::span<const PlannedPass>{}; }
std::span<const PlannedResource> CompiledGraph::resources() const noexcept { return state_ ? std::span<const PlannedResource>{state_->resources} : std::span<const PlannedResource>{}; }
std::span<const std::size_t> CompiledGraph::order() const noexcept { return state_ ? std::span<const std::size_t>{state_->order} : std::span<const std::size_t>{}; }
std::span<const Dependency> CompiledGraph::dependencies() const noexcept { return state_ ? std::span<const Dependency>{state_->dependencies} : std::span<const Dependency>{}; }
std::span<const TransientAllocation> CompiledGraph::allocations() const noexcept { return state_ ? std::span<const TransientAllocation>{state_->allocations} : std::span<const TransientAllocation>{}; }
} // namespace dk::graphics::graph
