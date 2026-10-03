#include <dk/graphics/GraphDiagnostics.hpp>
#include <dk/memory/MemorySystem.hpp>
#include <catch2/catch_test_macros.hpp>
#include <array>
#include <vector>
using namespace dk::graphics;
using namespace dk::graphics::graph;
namespace {
template<class T> T take(dk::Result<T> result) {
    if (!result) { INFO(result.error().message); FAIL("unexpected graph diagnostic error"); }
    return std::move(*result);
}
}
TEST_CASE("graph report describes culling accesses dependencies and lifetimes deterministically") {
    auto system = std::move(dk::memory::MemorySystem::create().value());
    const auto heap = system.create_heap({"report",dk::memory::DomainCategory::render}).value();
    auto graph = take(Graph::create(heap));
    const auto buffer = take(graph.declare_buffer("buffer\"\nname",{16}));
    const auto image = take(graph.declare_image("image",{2,2}));
    const std::array write{Use{buffer,{{vk::PipelineStageFlagBits2::eCopy,vk::AccessFlagBits2::eTransferWrite},0,16,{},true}}};
    static_cast<void>(take(graph.add_pass({"dead",write})));
    static_cast<void>(take(graph.add_pass({"producer",write})));
    const std::array copy{Use{buffer,{{vk::PipelineStageFlagBits2::eCopy,vk::AccessFlagBits2::eTransferRead}}},
        Use{image,{{vk::PipelineStageFlagBits2::eCopy,vk::AccessFlagBits2::eTransferWrite,vk::ImageLayout::eTransferDstOptimal},0,VK_WHOLE_SIZE,
            {vk::ImageAspectFlagBits::eColor,0,1,0,1},true}}};
    static_cast<void>(take(graph.add_pass({"consumer",copy})));
    REQUIRE(graph.mark_output(image));
    const auto plan = take(graph.compile());
    auto report = take(format_plan(plan));
    CHECK(report.text() == take(format_plan(plan)).text());
    CHECK(report.text().find("pass #0 \"dead\" culled") != std::string_view::npos);
    CHECK(report.text().find("order: 1 2") != std::string_view::npos);
    CHECK(report.text().find("buffer\\\"\\nname") != std::string_view::npos);
    CHECK(report.text().find("bytes=0+16") != std::string_view::npos);
    CHECK(report.text().find("mip=0+1 layer=0+1") != std::string_view::npos);
    CHECK(report.text().find("edge 1 -> 2 RAW resource=0") != std::string_view::npos);
    CHECK(report.text().find("edge 1 -> 2 contents resource=0") != std::string_view::npos);
    CHECK(report.text().find("allocate #0 before=0 release-after=1") != std::string_view::npos);
    CHECK(report.text().find("allocate #1 before=1 release-after=-") != std::string_view::npos);
    CHECK(report.text().find("0x") != std::string_view::npos);
    auto moved = std::move(report); CHECK(report.text().empty()); CHECK_FALSE(moved.text().empty());
}
TEST_CASE("graph report owns text after plan destruction and Memory close") {
    auto system = std::move(dk::memory::MemorySystem::create().value());
    const auto heap = system.create_heap({"report-close",dk::memory::DomainCategory::render}).value();
    PlanReport report;
    CHECK_FALSE(format_plan(CompiledGraph{}));
    {
        auto graph = take(Graph::create(heap));
        auto plan = take(graph.compile());
        report = take(format_plan(plan));
        heap.begin_close();
        CHECK_FALSE(format_plan(plan));
    }
    CHECK(report.text().find("passes=0 retained=0 resources=0") != std::string_view::npos);
    report = {}; CHECK(heap.snapshot().live_allocations == 0); CHECK(heap.try_close().closed());
}
TEST_CASE("graph report partial allocation failure leaves prior report and plan intact") {
    auto system = std::move(dk::memory::MemorySystem::create().value());
    const auto heap = system.create_heap({"report-budget",dk::memory::DomainCategory::render,32768}).value();
    auto graph = take(Graph::create(heap));
    for (unsigned i=0; i<10; ++i) static_cast<void>(take(graph.add_pass({"pass-"+std::to_string(i),{},true})));
    const auto plan = take(graph.compile()); const auto prior = take(format_plan(plan));
    struct Block { void* pointer; std::size_t size; }; std::vector<Block> padding;
    while (auto block = heap.try_allocate(64)) padding.push_back({*block,64});
    while (auto block = heap.try_allocate(1)) padding.push_back({*block,1});
    bool failed=false, partial=false, succeeded=false;
    do {
        const auto before = heap.snapshot();
        {
            const auto report = format_plan(plan);
            if (!report) { failed=true; partial |= heap.snapshot().allocation_count > before.allocation_count+2; }
            else { succeeded=true; CHECK(report->text() == prior.text()); }
        }
        CHECK(heap.snapshot().live_allocations == before.live_allocations);
        CHECK(heap.snapshot().backing_requested_bytes == before.backing_requested_bytes);
        if (succeeded || padding.empty()) break;
        const auto block = padding.back(); padding.pop_back(); heap.deallocate(block.pointer,block.size);
    } while (true);
    for (const auto& block : padding) heap.deallocate(block.pointer,block.size);
    CHECK(failed); CHECK(partial); CHECK(succeeded); CHECK(plan.order().size() == 10);
}
