#pragma once
#include <dk/graphics/Graph.hpp>

namespace dk::graphics::graph {
namespace detail { struct ReportState; }
// Independent diagnostic text; formatting is not a persistence protocol.
class PlanReport final {
public:
    PlanReport() = default;
    PlanReport(PlanReport&&) noexcept = default;
    PlanReport& operator=(PlanReport&&) noexcept = default;
    PlanReport(const PlanReport&) = delete;
    PlanReport& operator=(const PlanReport&) = delete;
    [[nodiscard]] std::string_view text() const noexcept;
private:
    friend Result<PlanReport> format_plan(const CompiledGraph& plan);
    explicit PlanReport(std::shared_ptr<detail::ReportState> state) : state_(std::move(state)) {}
    std::shared_ptr<detail::ReportState> state_;
};
[[nodiscard]] Result<PlanReport> format_plan(const CompiledGraph& plan);
} // namespace dk::graphics::graph
