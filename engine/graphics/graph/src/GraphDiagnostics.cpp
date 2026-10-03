#include <dk/graphics/GraphDiagnostics.hpp>
#include "GraphPlanInternal.hpp"
#include <charconv>

namespace dk::graphics::graph {
namespace detail {
struct ReportState {
    explicit ReportState(memory::ResourceHandle heap) : text(0,' ',memory::Allocator<char>{heap}) {}
    String text;
};
}
std::string_view PlanReport::text() const noexcept { return state_ ? std::string_view{state_->text} : std::string_view{}; }
namespace {
struct Writer {
    String& text;
    Writer& add(std::string_view value) { text.append(value); return *this; }
    template<class T> Writer& number(T value, int base = 10) {
        std::array<char,32> buffer{};
        const auto result = std::to_chars(buffer.data(),buffer.data()+buffer.size(),value,base);
        text.append(buffer.data(),static_cast<std::size_t>(result.ptr-buffer.data())); return *this;
    }
    Writer& optional(std::optional<std::size_t> value) { return value ? number(*value) : add("-"); }
    Writer& name(std::string_view value) {
        add("\"");
        for (const auto c : value) {
            switch (c) {
            case '\n': add("\\n"); break;
            case '\r': add("\\r"); break;
            case '\t': add("\\t"); break;
            case '"': add("\\\""); break;
            case '\\': add("\\\\"); break;
            default: if (static_cast<unsigned char>(c) < 32) { add("\\x"); number(static_cast<unsigned char>(c),16); }
                     else text.push_back(c);
            }
        }
        return add("\"");
    }
};
std::string_view kind(DependencyKind value) {
    switch (value) {
    case DependencyKind::explicit_order: return "explicit";
    case DependencyKind::read_after_write: return "RAW";
    case DependencyKind::write_after_read: return "WAR";
    case DependencyKind::write_after_write: return "WAW";
    case DependencyKind::layout_transition: return "layout";
    case DependencyKind::contents: return "contents";
    }
    return "unknown";
}
}
Result<PlanReport> format_plan(const CompiledGraph& plan) {
    if (!plan) return std::unexpected(Error{ErrorCode::invalid_state,"cannot format an empty plan owner"});
    const auto heap = detail::PlanAccess::state(plan)->resource;
    if (heap.state() != memory::ResourceState::open) return std::unexpected(Error{ErrorCode::invalid_state,"plan Memory resource is closed"});
    try {
        auto report = memory::make_shared_in<detail::ReportState>(heap,heap);
        Writer out{report->text};
        out.add("GPU Graph: passes=").number(plan.passes().size()).add(" retained=").number(plan.order().size())
            .add(" resources=").number(plan.resources().size()).add("\norder:");
        for (const auto index : plan.order()) out.add(" ").number(index);
        out.add("\n");
        for (std::size_t i=0; i<plan.passes().size(); ++i) {
            const auto& pass = plan.passes()[i];
            out.add("pass #").number(i).add(" ").name(pass.name).add(pass.retained ? " retained" : " culled")
                .add(" order=").optional(pass.order_index).add(pass.side_effect ? " side-effect\n" : "\n");
            for (const auto& use : pass.uses) {
                const auto& access = use.access;
                out.add("  use #").number(use.resource).add(" stages=0x").number(static_cast<VkPipelineStageFlags2>(access.state.stages),16)
                    .add(" access=0x").number(static_cast<VkAccessFlags2>(access.state.access),16);
                if (std::holds_alternative<BufferDesc>(plan.resources()[use.resource].description))
                    out.add(" bytes=").number(access.offset).add("+").number(access.size);
                else out.add(" layout=").number(static_cast<int>(access.state.layout)).add(" aspect=0x")
                    .number(static_cast<VkImageAspectFlags>(access.range.aspectMask),16).add(" mip=").number(access.range.baseMipLevel)
                    .add("+").number(access.range.levelCount).add(" layer=").number(access.range.baseArrayLayer).add("+").number(access.range.layerCount);
                out.add(access.full_overwrite ? " overwrite\n" : " preserve\n");
            }
        }
        for (std::size_t i=0; i<plan.resources().size(); ++i) {
            const auto& resource = plan.resources()[i];
            out.add("resource #").number(i).add(" ").name(resource.name);
            if (const auto* buffer = std::get_if<BufferDesc>(&resource.description)) out.add(" buffer bytes=").number(buffer->size);
            else { const auto& image = std::get<ImageDesc>(resource.description);
                out.add(" image extent=").number(image.width).add("x").number(image.height).add(" mips=").number(image.mip_levels).add(" layers=").number(image.array_layers); }
            out.add(resource.lifetime == Lifetime::transient ? " transient" : " external").add(resource.retained ? " retained" : " culled")
                .add(resource.output ? " output" : "").add(resource.initialized ? " initialized" : " uninitialized")
                .add(" first=").optional(resource.first_use).add(" last=").optional(resource.last_use).add(" allocation=").optional(resource.allocation_index).add("\n");
        }
        for (const auto& edge : plan.dependencies()) out.add("edge ").number(edge.before).add(" -> ").number(edge.after)
            .add(" ").add(kind(edge.kind)).add(" resource=").optional(edge.resource).add("\n");
        for (const auto& allocation : plan.allocations()) out.add("allocate #").number(allocation.resource).add(" before=").number(allocation.create_before)
            .add(" release-after=").optional(allocation.release_after).add("\n");
        return PlanReport{std::move(report)};
    } catch (const std::bad_alloc&) { return std::unexpected(Error{ErrorCode::internal_error,"graph report allocation failed"}); }
}
} // namespace dk::graphics::graph
