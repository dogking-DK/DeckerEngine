#include "GraphInternal.hpp"
#include <algorithm>
#include <new>
#include <tuple>

namespace dk::graphics::graph {

namespace {
using detail::GraphState;
using detail::HandleAccess;
Error invalid(std::string message) { return {ErrorCode::invalid_argument, std::move(message)}; }
Error allocation_failure() { return {ErrorCode::internal_error, "graph allocation failed; declaration unchanged"}; }
Result<void> ready(const std::shared_ptr<GraphState>& state) {
    if (!state || state->resource.state() != memory::ResourceState::open)
        return std::unexpected(Error{ErrorCode::invalid_state, "graph is empty, moved from, or its memory domain is closing"});
    return {};
}
bool valid_name(std::string_view name) { return !name.empty() && name.find('\0') == std::string_view::npos; }
template<class Items> bool has_name(const Items& items, std::string_view name) {
    return std::ranges::any_of(items, [&](const auto& item) { return std::string_view{item->name} == name; });
}
Result<void> resource_name(const GraphState& state, std::string_view name, Lifetime lifetime, bool initialized) {
    if (!valid_name(name)) return std::unexpected(invalid("resource requires a nonempty name without NUL"));
    if (has_name(state.buffers, name) || has_name(state.images, name))
        return std::unexpected(Error{ErrorCode::conflict, "duplicate resource name: " + std::string{name}});
    if ((lifetime != Lifetime::external && lifetime != Lifetime::transient) || (lifetime == Lifetime::transient && initialized))
        return std::unexpected(invalid("invalid lifetime or initialized transient resource: " + std::string{name}));
    return {};
}
template<class Tag> Result<std::size_t> index_of(const Id<Tag>& id, const std::shared_ptr<GraphState>& state, std::size_t size) {
    if (!HandleAccess::belongs(id, state) || HandleAccess::index(id) >= size)
        return std::unexpected(invalid("invalid, expired or foreign graph handle"));
    return HandleAccess::index(id);
}
Result<std::size_t> resource_index(const ResourceId& id, const std::shared_ptr<GraphState>& state) {
    if (const auto* buffer = std::get_if<BufferId>(&id)) return index_of(*buffer, state, state->buffers.size());
    return index_of(std::get<ImageId>(id), state, state->images.size());
}
std::string resource_context(const ResourceId& id, const GraphState& state) {
    return std::visit([&](const auto& value) {
        const auto i = HandleAccess::index(value);
        if constexpr (std::is_same_v<std::decay_t<decltype(value)>, BufferId>)
            return "buffer[" + std::to_string(i) + "] '" + std::string{state.buffers[i]->name} + "'";
        else return "image[" + std::to_string(i) + "] '" + std::string{state.images[i]->name} + "'";
    }, id);
}
std::string pass_context(const GraphState& state, std::size_t index) {
    return "pass[" + std::to_string(index) + "] '" + std::string{state.passes[index]->name} + "'";
}
bool intersects(const vk::ImageSubresourceRange& a, const vk::ImageSubresourceRange& b) {
    // All ranges have already been checked without overflowing addition.
    return a.baseMipLevel < b.baseMipLevel + b.levelCount && b.baseMipLevel < a.baseMipLevel + a.levelCount &&
        a.baseArrayLayer < b.baseArrayLayer + b.layerCount && b.baseArrayLayer < a.baseArrayLayer + a.layerCount;
}
bool overlaps(const Use& a, const Use& b) {
    return a.resource == b.resource && (std::holds_alternative<BufferId>(a.resource) || intersects(a.access.range, b.access.range));
}
Result<Use> normalize_use(const Use& input, const std::shared_ptr<GraphState>& state) {
    const auto i = resource_index(input.resource, state);
    if (!i) return std::unexpected(i.error());
    const auto& access = input.access;
    if (access.state.initialized || (access.state.stages & vk::PipelineStageFlagBits2::eHost) ||
        (access.state.access & (vk::AccessFlagBits2::eHostRead | vk::AccessFlagBits2::eHostWrite)))
        return std::unexpected(invalid("GPU Pass cannot declare host access or set tracked initialized state"));
    if (access.full_overwrite && access_reads(access.state.access))
        return std::unexpected(invalid("full overwrite cannot also read prior contents"));
    auto checked = std::holds_alternative<BufferId>(input.resource)
        ? validate_buffer_access(state->buffers[*i]->description, access)
        : validate_image_access(state->images[*i]->description, access);
    if (!checked) return std::unexpected(checked.error().with_context(resource_context(input.resource, *state)));
    return Use{input.resource, *checked};
}
// Coverage uses interval unions, never a byte/pixel/layer-sized allocation.
// For images, each mip (at most 32) is a separate interval of array layers.
bool initialized(const Use& requested, std::span<const Use* const> writes, const GraphState& state) {
    const bool buffer = std::holds_alternative<BufferId>(requested.resource);
    const auto index = std::visit([](const auto& id) { return HandleAccess::index(id); }, requested.resource);
    if (buffer ? state.buffers[index]->initialized : state.images[index]->initialized) return true;
    const auto& range = requested.access.range;
    const std::uint32_t first_mip = buffer ? 0 : range.baseMipLevel;
    const std::uint32_t end_mip = buffer ? 1 : range.baseMipLevel + range.levelCount;
    for (auto mip = first_mip; mip < end_mip; ++mip) {
        vk::DeviceSize cursor = buffer ? requested.access.offset : range.baseArrayLayer;
        const vk::DeviceSize end = cursor + (buffer ? requested.access.size : range.layerCount);
        while (cursor < end) {
            auto covered = cursor;
            for (const auto* write : writes) {
                if (write->resource != requested.resource) continue;
                const auto& access = write->access;
                if (!buffer && (mip < access.range.baseMipLevel || mip >= access.range.baseMipLevel + access.range.levelCount)) continue;
                const vk::DeviceSize begin = buffer ? access.offset : access.range.baseArrayLayer;
                const vk::DeviceSize finish = begin + (buffer ? access.size : access.range.layerCount);
                if (begin <= cursor) covered = std::max(covered, finish);
            }
            if (covered == cursor) return false;
            cursor = covered;
        }
    }
    return true;
}
Use whole_resource(const ResourceId& resource, const GraphState& state) {
    Use use{resource, {}};
    const auto index = std::visit([](const auto& id) { return HandleAccess::index(id); }, resource);
    if (std::holds_alternative<BufferId>(resource)) use.access.size = state.buffers[index]->description.size;
    else {
        const auto& desc = state.images[index]->description;
        use.access.range = vk::ImageSubresourceRange{desc.format == vk::Format::eD32Sfloat ? vk::ImageAspectFlagBits::eDepth : vk::ImageAspectFlagBits::eColor,
            0, desc.mip_levels, 0, desc.array_layers};
    }
    return use;
}
Result<void> acyclic(const GraphState& state, std::span<const Dependency> edges) {
    struct Visit { std::size_t node, next_edge; };
    Vector<unsigned char> color(state.passes.size(), 0, memory::Allocator<unsigned char>{state.resource});
    Vector<Visit> stack(0, memory::Allocator<Visit>{state.resource});
    const auto first_edge = [&](std::size_t node) {
        return static_cast<std::size_t>(std::ranges::lower_bound(edges, node, {}, &Dependency::before) - edges.begin());
    };
    for (std::size_t root = 0; root < state.passes.size(); ++root) {
        if (color[root]) continue;
        stack.push_back({root, first_edge(root)}); color[root] = 1;
        while (!stack.empty()) {
            auto& current = stack.back();
            if (current.next_edge == edges.size() || edges[current.next_edge].before != current.node) { color[current.node] = 2; stack.pop_back(); continue; }
            const auto next = edges[current.next_edge++].after;
            if (color[next] == 2) continue;
            if (color[next] == 1) {
                std::string cycle = "graph dependency cycle: ";
                const auto start = std::ranges::find_if(stack, [&](const Visit& visit) { return visit.node == next; });
                for (auto it = start; it != stack.end(); ++it) cycle += pass_context(state, it->node) + " -> ";
                cycle += pass_context(state, next);
                return std::unexpected(Error{ErrorCode::conflict, std::move(cycle)});
            }
            stack.push_back({next, first_edge(next)}); color[next] = 1;
        }
    }
    return {};
}
} // namespace

Result<Graph> Graph::create(memory::ResourceHandle resource) try {
    if (!resource || resource.state() != memory::ResourceState::open)
        return std::unexpected(invalid("graph requires an open memory resource"));
    return Graph{memory::make_shared_in<detail::GraphState>(resource, resource)};
} catch (const std::bad_alloc&) { return std::unexpected(allocation_failure()); }

Result<BufferId> Graph::declare_buffer(std::string_view name, const BufferDesc& desc, Lifetime lifetime, bool contents) try {
    if (auto valid = ready(state_); !valid) return std::unexpected(valid.error());
    if (auto valid = resource_name(*state_, name, lifetime, contents); !valid) return std::unexpected(valid.error());
    if (auto valid = validate_buffer_description(desc); !valid) return std::unexpected(valid.error().with_context(std::string{name}));
    auto candidate = memory::make_unique_in<detail::Resource<BufferDesc>>(state_->resource, state_->resource, name, desc, lifetime, contents);
    state_->buffers.push_back(std::move(candidate));
    return HandleAccess::make<BufferTag>(state_, state_->buffers.size() - 1);
} catch (const std::bad_alloc&) { return std::unexpected(allocation_failure()); }

Result<ImageId> Graph::declare_image(std::string_view name, const ImageDesc& desc, Lifetime lifetime, bool contents) try {
    if (auto valid = ready(state_); !valid) return std::unexpected(valid.error());
    if (auto valid = resource_name(*state_, name, lifetime, contents); !valid) return std::unexpected(valid.error());
    if (auto valid = validate_image_description(desc); !valid) return std::unexpected(valid.error().with_context(std::string{name}));
    auto candidate = memory::make_unique_in<detail::Resource<ImageDesc>>(state_->resource, state_->resource, name, desc, lifetime, contents);
    state_->images.push_back(std::move(candidate));
    return HandleAccess::make<ImageTag>(state_, state_->images.size() - 1);
} catch (const std::bad_alloc&) { return std::unexpected(allocation_failure()); }

Result<PassId> Graph::add_pass(const PassDesc& desc) try {
    if (auto valid = ready(state_); !valid) return std::unexpected(valid.error());
    if (!valid_name(desc.name)) return std::unexpected(invalid("Pass requires a nonempty name without NUL"));
    if (has_name(state_->passes, desc.name)) return std::unexpected(Error{ErrorCode::conflict, "duplicate Pass name: " + std::string{desc.name}});
    auto candidate = memory::make_unique_in<detail::Pass>(state_->resource, state_->resource, desc);
    candidate->uses.reserve(desc.uses.size());
    for (const auto& use : desc.uses) {
        auto normalized = normalize_use(use, state_);
        if (!normalized) return std::unexpected(normalized.error().with_context("Pass '" + std::string{desc.name} + "' use[" + std::to_string(candidate->uses.size()) + "]"));
        if (std::ranges::any_of(candidate->uses, [&](const auto& prior) { return overlaps(prior, *normalized); }))
            return std::unexpected(invalid("Pass '" + std::string{desc.name} + "' has overlapping uses; merge the access declaration"));
        candidate->uses.push_back(std::move(*normalized));
    }
    state_->passes.push_back(std::move(candidate));
    return HandleAccess::make<PassTag>(state_, state_->passes.size() - 1);
} catch (const std::bad_alloc&) { return std::unexpected(allocation_failure()); }

Result<void> Graph::add_dependency(PassId before, PassId after) try {
    if (auto valid = ready(state_); !valid) return valid;
    const auto a = index_of(before, state_, state_->passes.size()), b = index_of(after, state_, state_->passes.size());
    if (!a) return std::unexpected(a.error());
    if (!b) return std::unexpected(b.error());
    if (*a == *b) return std::unexpected(invalid("self dependency: " + pass_context(*state_, *a)));
    const detail::Edge edge{*a, *b};
    if (std::ranges::find(state_->dependencies, edge) == state_->dependencies.end()) state_->dependencies.push_back(edge);
    return {};
} catch (const std::bad_alloc&) { return std::unexpected(allocation_failure()); }

Result<void> Graph::remove_dependency(PassId before, PassId after) {
    if (auto valid = ready(state_); !valid) return valid;
    const auto a = index_of(before, state_, state_->passes.size()), b = index_of(after, state_, state_->passes.size());
    if (!a) return std::unexpected(a.error());
    if (!b) return std::unexpected(b.error());
    std::erase(state_->dependencies, detail::Edge{*a, *b});
    return {};
}
Result<void> Graph::mark_output(ResourceId resource) try {
    if (auto valid = ready(state_); !valid) return valid;
    if (auto index = resource_index(resource, state_); !index) return std::unexpected(index.error());
    if (std::ranges::find(state_->outputs, resource) == state_->outputs.end()) state_->outputs.push_back(std::move(resource));
    return {};
} catch (const std::bad_alloc&) { return std::unexpected(allocation_failure()); }

Result<BufferInfo> Graph::buffer(BufferId id) const {
    if (auto valid = ready(state_); !valid) return std::unexpected(valid.error());
    const auto i = index_of(id, state_, state_->buffers.size());
    if (!i) return std::unexpected(i.error());
    const auto& value = *state_->buffers[*i];
    return BufferInfo{value.name, value.description, value.lifetime, value.initialized};
}
Result<ImageInfo> Graph::image(ImageId id) const {
    if (auto valid = ready(state_); !valid) return std::unexpected(valid.error());
    const auto i = index_of(id, state_, state_->images.size());
    if (!i) return std::unexpected(i.error());
    const auto& value = *state_->images[*i];
    return ImageInfo{value.name, value.description, value.lifetime, value.initialized};
}
Result<PassDesc> Graph::pass(PassId id) const {
    if (auto valid = ready(state_); !valid) return std::unexpected(valid.error());
    const auto i = index_of(id, state_, state_->passes.size());
    if (!i) return std::unexpected(i.error());
    const auto& value = *state_->passes[*i];
    return PassDesc{value.name, value.uses, value.side_effect};
}
Counts Graph::counts() const noexcept {
    if (!state_) return {};
    return {state_->buffers.size(), state_->images.size(), state_->passes.size(), state_->dependencies.size(), state_->outputs.size()};
}
Result<void> Graph::reset() {
    if (auto valid = ready(state_); !valid) return valid;
    auto candidate = create(state_->resource);
    if (!candidate) return std::unexpected(candidate.error());
    state_.swap(candidate->state_);
    return {};
}
Result<void> detail::analyze(const std::shared_ptr<GraphState>& state, Vector<Dependency>& edges, std::stop_token stop) {
    if (auto valid = ready(state); !valid) return valid;
    Vector<const Use*> writes(0, memory::Allocator<const Use*>{state->resource});
    for (std::size_t i = 0; i < state->passes.size(); ++i) {
        if (stop.stop_requested()) return std::unexpected(compilation_cancelled());
        const auto& pass = *state->passes[i];
        for (const auto& use : pass.uses) {
            if (!use.access.full_overwrite && !initialized(use, writes, *state))
                return std::unexpected(invalid("read or preserved write requires initialized contents")
                    .with_context(resource_context(use.resource, *state)).with_context(pass_context(*state, i)));
        }
        for (const auto& use : pass.uses) if (use.access.full_overwrite) writes.push_back(&use);
    }
    for (const auto& output : state->outputs) {
        if (!initialized(whole_resource(output, *state), writes, *state))
            return std::unexpected(invalid("graph output is not fully initialized").with_context(resource_context(output, *state)));
    }
    for (const auto& edge : state->dependencies)
        edges.push_back({edge.before, edge.after, DependencyKind::explicit_order, {}});
    for (std::size_t before = 0; before < state->passes.size(); ++before) {
        if (stop.stop_requested()) return std::unexpected(compilation_cancelled());
        for (std::size_t after = before + 1; after < state->passes.size(); ++after) {
            for (const auto& a : state->passes[before]->uses) {
                for (const auto& b : state->passes[after]->uses) {
                    if (!overlaps(a, b)) continue;
                    const auto resource = resource_number(a.resource, *state);
                    const auto add = [&](DependencyKind kind) { edges.push_back({before, after, kind, resource}); };
                    if (access_writes(a.access.state.access) && access_reads(b.access.state.access)) add(DependencyKind::read_after_write);
                    if (access_reads(a.access.state.access) && access_writes(b.access.state.access)) add(DependencyKind::write_after_read);
                    if (access_writes(a.access.state.access) && access_writes(b.access.state.access)) add(DependencyKind::write_after_write);
                    if (a.access.state.layout != b.access.state.layout) add(DependencyKind::layout_transition);
                }
            }
        }
    }
    if (stop.stop_requested()) return std::unexpected(compilation_cancelled());
    normalize_dependencies(edges,stop);
    if (stop.stop_requested()) return std::unexpected(compilation_cancelled());
    return acyclic(*state, edges);
}
void detail::normalize_dependencies(Vector<Dependency>& dependencies, std::stop_token stop) {
    const auto less = [](const Dependency& a, const Dependency& b) {
        return std::tie(a.before,a.after,a.kind,a.resource) < std::tie(b.before,b.after,b.kind,b.resource);
    };
    if (stop.stop_possible()) {
        std::size_t comparisons=0;
        std::ranges::sort(dependencies, [&](const Dependency& a, const Dependency& b) {
            if ((comparisons++ & 4095u)==0 && stop.stop_requested()) throw compilation_cancelled();
            return less(a,b);
        });
    } else std::ranges::sort(dependencies,less);
    dependencies.erase(std::unique(dependencies.begin(),dependencies.end()),dependencies.end());
}
std::size_t detail::resource_number(const ResourceId& id, const GraphState& state) {
    if (const auto* buffer = std::get_if<BufferId>(&id)) return HandleAccess::index(*buffer);
    return state.buffers.size() + HandleAccess::index(std::get<ImageId>(id));
}
Use detail::complete_use(const ResourceId& id, const GraphState& state) { return whole_resource(id, state); }
Result<void> Graph::validate() const try {
    if (auto valid = ready(state_); !valid) return valid;
    Vector<Dependency> dependencies(0, memory::Allocator<Dependency>{state_->resource});
    return detail::analyze(state_, dependencies);
} catch (const std::bad_alloc&) { return std::unexpected(allocation_failure()); }
} // namespace dk::graphics::graph
