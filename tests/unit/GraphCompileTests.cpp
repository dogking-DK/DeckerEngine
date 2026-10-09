#include <dk/graphics/Graph.hpp>
#include <dk/memory/MemorySystem.hpp>
#include <catch2/catch_test_macros.hpp>
#include <algorithm>
#include <array>
#include <string>
#include <vector>

using namespace dk::graphics;
using namespace dk::graphics::graph;
namespace {
struct Fixture {
    dk::memory::MemorySystem system = std::move(dk::memory::MemorySystem::create().value());
    dk::memory::ResourceHandle heap;
    Graph graph;
    explicit Fixture(std::size_t budget = 0) {
        heap = system.create_heap({"graph-compile-tests", dk::memory::DomainCategory::render, budget}).value();
        graph = std::move(Graph::create(heap).value());
    }
};
template<class T> T take(dk::Result<T> result) {
    if (!result) { INFO(result.error().message); FAIL("unexpected graph error"); }
    return std::move(*result);
}
Use read(BufferId id, vk::DeviceSize offset = 0, vk::DeviceSize size = VK_WHOLE_SIZE) {
    return {id, {{vk::PipelineStageFlagBits2::eCopy, vk::AccessFlagBits2::eTransferRead}, offset, size}};
}
Use write(BufferId id, bool overwrite = true, vk::DeviceSize offset = 0, vk::DeviceSize size = VK_WHOLE_SIZE) {
    return {id, {{vk::PipelineStageFlagBits2::eCopy, vk::AccessFlagBits2::eTransferWrite}, offset, size, {}, overwrite}};
}
Use image_write(ImageId id, vk::ImageSubresourceRange range = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1}) {
    return {id, {{vk::PipelineStageFlagBits2::eCopy, vk::AccessFlagBits2::eTransferWrite, vk::ImageLayout::eTransferDstOptimal}, 0, VK_WHOLE_SIZE, range, true}};
}
Use image_read(ImageId id, vk::ImageSubresourceRange range = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1}) {
    return {id, {{vk::PipelineStageFlagBits2::eCopy, vk::AccessFlagBits2::eTransferRead, vk::ImageLayout::eTransferSrcOptimal}, 0, VK_WHOLE_SIZE, range}};
}
PassId add(Graph& graph, std::string_view name, std::initializer_list<Use> uses = {}, bool effect = false) {
    return take(graph.add_pass({name, {uses.begin(), uses.size()}, effect}));
}
std::vector<std::string_view> names(const CompiledGraph& plan) {
    std::vector<std::string_view> result;
    for (const auto i : plan.order()) result.push_back(plan.passes()[i].name);
    return result;
}
bool edge(const CompiledGraph& plan, std::size_t before, std::size_t after, DependencyKind kind) {
    return std::ranges::any_of(plan.dependencies(), [&](const auto& e) { return e.before == before && e.after == after && e.kind == kind; });
}
void consistent(const CompiledGraph& plan) {
    for (const auto& dependency : plan.dependencies()) {
        REQUIRE(dependency.before < plan.passes().size()); REQUIRE(dependency.after < plan.passes().size());
        REQUIRE(plan.passes()[dependency.before].retained); REQUIRE(plan.passes()[dependency.after].retained);
        CHECK(plan.passes()[dependency.before].order_index < plan.passes()[dependency.after].order_index);
        if (dependency.resource) CHECK(*dependency.resource < plan.resources().size());
    }
    for (std::size_t i = 0; i < plan.order().size(); ++i) {
        CHECK(plan.passes()[plan.order()[i]].order_index == i);
        for (const auto& use : plan.passes()[plan.order()[i]].uses) {
            REQUIRE(use.resource < plan.resources().size());
            const auto& resource = plan.resources()[use.resource];
            CHECK(resource.retained);
            REQUIRE(resource.first_use); REQUIRE(resource.last_use);
            CHECK(*resource.first_use <= i); CHECK(*resource.last_use >= i);
        }
    }
}
}

TEST_CASE("graph compile plans upload compute draw and readback") {
    Fixture f;
    auto& g = f.graph;
    const auto upload = take(g.declare_buffer("upload", {64}, Lifetime::external, true));
    const auto mesh = take(g.declare_buffer("mesh", {64, vk::BufferUsageFlagBits::eTransferDst | vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eVertexBuffer}));
    const auto color = take(g.declare_image("color", {16, 16, vk::Format::eR8G8B8A8Unorm, vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eTransferSrc}));
    const auto output = take(g.declare_buffer("readback", {1024, vk::BufferUsageFlagBits::eTransferDst, BufferMemory::readback}, Lifetime::external));
    add(g, "upload", {read(upload), write(mesh)});
    add(g, "compute", {{mesh, {{vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderStorageRead | vk::AccessFlagBits2::eShaderStorageWrite}}}});
    add(g, "draw", {{mesh, {{vk::PipelineStageFlagBits2::eVertexAttributeInput, vk::AccessFlagBits2::eVertexAttributeRead}}},
        {color, {{vk::PipelineStageFlagBits2::eColorAttachmentOutput, vk::AccessFlagBits2::eColorAttachmentWrite, vk::ImageLayout::eColorAttachmentOptimal}, 0, VK_WHOLE_SIZE,
            {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1}, true}}});
    add(g, "readback", {image_read(color), write(output)});
    REQUIRE(g.mark_output(output));
    auto plan = take(g.compile());
    REQUIRE(plan);
    CHECK(names(plan) == std::vector<std::string_view>{"upload", "compute", "draw", "readback"});
    REQUIRE(plan.resources().size() == 4);
    CHECK(plan.resources()[0].name == "upload"); CHECK(plan.resources()[2].name == "readback"); CHECK(plan.resources()[3].name == "color");
    CHECK(std::get<ImageDesc>(plan.resources()[3].description).width == 16);
    CHECK(plan.resources()[3].declaration_index == 0);
    CHECK(plan.passes()[0].uses[0].access.size == 64);
    CHECK(edge(plan, 0, 1, DependencyKind::contents)); CHECK(edge(plan, 1, 2, DependencyKind::contents));
    CHECK(edge(plan, 2, 3, DependencyKind::layout_transition));
    REQUIRE(plan.allocations().size() == 2);
    CHECK(plan.allocations()[0] == TransientAllocation{1, 0, 2});
    CHECK(plan.allocations()[1] == TransientAllocation{3, 2, 3});
    CHECK_FALSE(plan.resources()[2].allocation_index);
    consistent(plan);
}

TEST_CASE("graph compile stable ordering chooses smallest ready declaration") {
    Fixture f;
    const auto a = add(f.graph, "A", {}, true);
    add(f.graph, "B", {}, true);
    const auto c = add(f.graph, "C", {}, true);
    add(f.graph, "unused");
    REQUIRE(f.graph.add_dependency(c, a));
    const auto first = take(f.graph.compile()), second = take(f.graph.compile());
    CHECK(names(first) == std::vector<std::string_view>{"B", "C", "A"});
    CHECK(names(first) == names(second));
    CHECK(std::ranges::equal(first.dependencies(), second.dependencies()));
    CHECK_FALSE(first.passes()[3].retained); CHECK_FALSE(first.passes()[3].order_index);
    CHECK(first.allocations().empty()); consistent(first);
}

TEST_CASE("graph compile distinguishes empty plans and culls graphs without roots") {
    Fixture f;
    CompiledGraph missing;
    CHECK_FALSE(missing); CHECK(missing.passes().empty()); CHECK(missing.resources().empty());
    CHECK(missing.dependencies().empty()); CHECK(missing.order().empty()); CHECK(missing.allocations().empty());
    auto empty = take(f.graph.compile()); CHECK(empty); CHECK(empty.order().empty());
    const auto b = take(f.graph.declare_buffer("unused transient", {64}));
    const auto external = take(f.graph.declare_buffer("unobserved external", {64}, Lifetime::external));
    add(f.graph, "write", {write(b)});
    add(f.graph, "read", {read(b)});
    add(f.graph, "unmarked external write", {write(external)});
    const auto plan = take(f.graph.compile());
    CHECK(plan.order().empty()); CHECK(plan.dependencies().empty()); CHECK(plan.allocations().empty());
    for (const auto& resource : plan.resources()) { CHECK_FALSE(resource.retained); CHECK_FALSE(resource.first_use); }
    for (const auto& pass : plan.passes()) CHECK_FALSE(pass.retained);
}

TEST_CASE("graph compile discards overwritten producers and unrelated branches") {
    Fixture f;
    const auto output = take(f.graph.declare_buffer("output", {64}));
    const auto unused = take(f.graph.declare_buffer("unused", {64}));
    add(f.graph, "obsolete", {write(output)});
    add(f.graph, "unused producer", {write(unused)});
    add(f.graph, "unused consumer", {read(unused)});
    add(f.graph, "final", {write(output)});
    REQUIRE(f.graph.mark_output(output));
    const auto plan = take(f.graph.compile());
    CHECK(names(plan) == std::vector<std::string_view>{"final"});
    CHECK(plan.dependencies().empty());
    REQUIRE(plan.allocations().size() == 1);
    CHECK(plan.allocations()[0] == TransientAllocation{0, 0, {}});
    CHECK(plan.resources()[0].output); CHECK(plan.resources()[0].first_use == 0); CHECK(plan.resources()[0].last_use == 0);
    CHECK_FALSE(plan.resources()[1].retained); consistent(plan);
}

TEST_CASE("graph compile preserves surviving RAW WAR and WAW order") {
    Fixture f;
    const auto b = take(f.graph.declare_buffer("b", {64}));
    add(f.graph, "old value", {write(b)});
    add(f.graph, "observe old", {read(b)}, true);
    add(f.graph, "new value", {write(b)});
    REQUIRE(f.graph.mark_output(b));
    const auto plan = take(f.graph.compile());
    CHECK(names(plan) == std::vector<std::string_view>{"old value", "observe old", "new value"});
    CHECK(edge(plan, 0, 1, DependencyKind::read_after_write));
    CHECK(edge(plan, 1, 2, DependencyKind::write_after_read));
    CHECK(edge(plan, 0, 2, DependencyKind::write_after_write));
    consistent(plan);
}

TEST_CASE("graph compile retains explicit predecessors even when their data is overwritten") {
    Fixture f;
    const auto b = take(f.graph.declare_buffer("b", {64}));
    const auto setup = add(f.graph, "setup");
    const auto old = add(f.graph, "old", {write(b)});
    const auto final = add(f.graph, "final", {write(b)});
    REQUIRE(f.graph.add_dependency(setup, old)); REQUIRE(f.graph.add_dependency(old, final));
    REQUIRE(f.graph.add_dependency(old, final));
    REQUIRE(f.graph.mark_output(b));
    const auto plan = take(f.graph.compile());
    CHECK(names(plan) == std::vector<std::string_view>{"setup", "old", "final"});
    CHECK(edge(plan, 0, 1, DependencyKind::explicit_order)); CHECK(edge(plan, 1, 2, DependencyKind::explicit_order));
    CHECK(std::ranges::count(plan.dependencies(), Dependency{1, 2, DependencyKind::explicit_order, {}}) == 1);
    consistent(plan);
}

TEST_CASE("graph compile finds all final byte interval producers") {
    Fixture f;
    const auto b = take(f.graph.declare_buffer("b", {64}));
    add(f.graph, "obsolete full write", {write(b)});
    add(f.graph, "middle", {write(b, true, 16, 32)});
    add(f.graph, "left", {write(b, true, 0, 16)});
    add(f.graph, "right", {write(b, true, 48, 16)});
    REQUIRE(f.graph.mark_output(b));
    const auto plan = take(f.graph.compile());
    CHECK(names(plan) == std::vector<std::string_view>{"middle", "left", "right"});
    CHECK_FALSE(plan.passes()[0].retained);
    CHECK(plan.resources()[0].first_use == 0); CHECK(plan.resources()[0].last_use == 2);
    consistent(plan);
}

TEST_CASE("graph compile content culling respects exact buffer ranges and preserving writes") {
    Fixture f;
    const auto b = take(f.graph.declare_buffer("b", {64}));
    SECTION("unread other half does not retain its writer") {
        add(f.graph, "left", {write(b, true, 0, 32)});
        add(f.graph, "right", {write(b, true, 32, 32)});
        add(f.graph, "observe left", {read(b, 0, 32)}, true);
        const auto plan = take(f.graph.compile());
        CHECK(names(plan) == std::vector<std::string_view>{"left", "observe left"});
        CHECK_FALSE(plan.passes()[1].retained); consistent(plan);
    }
    SECTION("preserving write needs the previous value") {
        add(f.graph, "initial", {write(b)});
        add(f.graph, "partial", {write(b, false, 16, 32)});
        add(f.graph, "observe", {read(b, 16, 32)}, true);
        const auto plan = take(f.graph.compile());
        CHECK(names(plan) == std::vector<std::string_view>{"initial", "partial", "observe"});
        CHECK(edge(plan, 0, 1, DependencyKind::contents)); CHECK(edge(plan, 1, 2, DependencyKind::contents));
        CHECK_FALSE(edge(plan, 0, 2, DependencyKind::contents)); consistent(plan);
    }
}

TEST_CASE("graph compile follows independent mip and layer producers") {
    Fixture f;
    const auto image = take(f.graph.declare_image("array", {4, 4, vk::Format::eR8G8B8A8Unorm,
        vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eTransferDst, 2, 2}));
    const vk::ImageSubresourceRange all{vk::ImageAspectFlagBits::eColor, 0, 2, 0, 2};
    add(f.graph, "obsolete all", {image_write(image, all)});
    for (std::uint32_t mip = 0; mip < 2; ++mip)
        for (std::uint32_t layer = 0; layer < 2; ++layer)
            add(f.graph, "cell" + std::to_string(mip * 2 + layer), {image_write(image, {vk::ImageAspectFlagBits::eColor, mip, 1, layer, 1})});
    SECTION("whole image keeps four producers") {
        add(f.graph, "observe", {image_read(image, all)}, true);
        REQUIRE(f.graph.mark_output(image));
        const auto plan = take(f.graph.compile());
        CHECK_FALSE(plan.passes()[0].retained); CHECK(plan.order().size() == 5);
        for (std::size_t p = 1; p <= 4; ++p) CHECK(edge(plan, p, 5, DependencyKind::contents));
        CHECK(plan.resources()[0].first_use == 0); CHECK(plan.resources()[0].last_use == 4); consistent(plan);
    }
    SECTION("one subresource keeps only its producer") {
        add(f.graph, "observe", {image_read(image, {vk::ImageAspectFlagBits::eColor, 1, 1, 0, 1})}, true);
        const auto plan = take(f.graph.compile());
        CHECK(names(plan) == std::vector<std::string_view>{"cell2", "observe"}); consistent(plan);
    }
}

TEST_CASE("graph compile layout ordering does not retain unused readers") {
    Fixture f;
    const auto image = take(f.graph.declare_image("external", {4, 4}, Lifetime::external, true));
    auto second = image_read(image); second.access.state.layout = vk::ImageLayout::eGeneral;
    SECTION("unused first reader is culled") {
        add(f.graph, "unused", {image_read(image)});
        add(f.graph, "observer", {second}, true);
        const auto plan = take(f.graph.compile());
        CHECK(names(plan) == std::vector<std::string_view>{"observer"}); CHECK(plan.dependencies().empty());
    }
    SECTION("two observed readers retain layout ordering") {
        add(f.graph, "first", {image_read(image)}, true);
        add(f.graph, "second", {second}, true);
        const auto plan = take(f.graph.compile());
        CHECK(edge(plan, 0, 1, DependencyKind::layout_transition)); consistent(plan);
    }
}

TEST_CASE("graph compile keeps imported output without allocating or inventing passes") {
    Fixture f;
    const auto imported = take(f.graph.declare_buffer("imported", {64}, Lifetime::external, true));
    take(f.graph.declare_image("unused", {4, 4}));
    REQUIRE(f.graph.mark_output(imported));
    const auto plan = take(f.graph.compile());
    CHECK(plan.order().empty()); CHECK(plan.allocations().empty());
    REQUIRE(plan.resources().size() == 2);
    const auto& resource = plan.resources()[0];
    CHECK(resource.retained); CHECK(resource.output); CHECK(resource.initialized);
    CHECK_FALSE(resource.first_use); CHECK_FALSE(resource.last_use); CHECK_FALSE(resource.allocation_index);
    CHECK_FALSE(plan.resources()[1].retained);
}

TEST_CASE("graph compile allocation lifetimes use schedule positions and never alias") {
    Fixture f;
    const auto a = take(f.graph.declare_buffer("A", {64})), b = take(f.graph.declare_buffer("B", {64}));
    const auto write_a = add(f.graph, "write A", {write(a)});
    add(f.graph, "read A", {read(a)}, true);
    const auto write_b = add(f.graph, "write B", {write(b)}, true);
    REQUIRE(f.graph.add_dependency(write_b, write_a));
    auto plan = take(f.graph.compile());
    CHECK(names(plan) == std::vector<std::string_view>{"write B", "write A", "read A"});
    REQUIRE(plan.allocations().size() == 2);
    CHECK(plan.allocations()[0] == TransientAllocation{1, 0, 0});
    CHECK(plan.allocations()[1] == TransientAllocation{0, 1, 2});
    CHECK(plan.resources()[0].allocation_index == 1); CHECK(plan.resources()[1].allocation_index == 0);
    CHECK(plan.resources()[0].first_use == 1); CHECK(plan.resources()[0].last_use == 2);
    consistent(plan);
    REQUIRE(f.graph.mark_output(a));
    const auto exported = take(f.graph.compile());
    CHECK_FALSE(exported.allocations()[1].release_after);
    CHECK(plan.allocations()[1].release_after == 2); // Earlier snapshot remains unchanged.
}

TEST_CASE("graph compile rejects invalid dead branches before culling") {
    Fixture f;
    add(f.graph, "root", {}, true);
    const auto good = take(f.graph.compile());
    SECTION("undefined contents in dead pass") {
        const auto b = take(f.graph.declare_buffer("b", {64}));
        add(f.graph, "bad read", {read(b)});
        const auto result = f.graph.compile();
        REQUIRE_FALSE(result); CHECK(result.error().code == dk::ErrorCode::invalid_argument);
    }
    SECTION("cycle in dead passes") {
        const auto a = add(f.graph, "A"), b = add(f.graph, "B");
        REQUIRE(f.graph.add_dependency(a, b)); REQUIRE(f.graph.add_dependency(b, a));
        const auto result = f.graph.compile();
        REQUIRE_FALSE(result); CHECK(result.error().code == dk::ErrorCode::conflict);
        CHECK(result.error().message.find("'A'") != std::string::npos);
    }
    CHECK(names(good) == std::vector<std::string_view>{"root"});
}

TEST_CASE("graph compile snapshots survive graph mutation reset destruction and move") {
    Fixture f;
    const auto id = take(f.graph.declare_buffer(std::string(100, 'r'), {64}));
    add(f.graph, std::string(100, 'p'), {write(id)}, true);
    auto first = take(f.graph.compile());
    add(f.graph, "later", {}, true);
    const auto second = take(f.graph.compile());
    CHECK(first.passes().size() == 1); CHECK(second.passes().size() == 2);
    REQUIRE(f.graph.reset()); CHECK_FALSE(id);
    f.graph = Graph{};
    auto moved = std::move(first);
    CHECK_FALSE(first); CHECK(first.passes().empty()); CHECK(first.resources().empty());
    CHECK(moved.passes()[0].name == std::string(100, 'p'));
    CHECK(moved.resources()[0].name == std::string(100, 'r'));
    CHECK(moved.passes()[0].uses[0].access.size == 64);
    CHECK(std::get<BufferDesc>(moved.resources()[0].description).size == 64);
    CHECK(second.passes()[1].name == "later"); consistent(moved); consistent(second);
    CHECK_FALSE(f.graph.compile());
}

TEST_CASE("graph compile plan reads survive domain close and release all owned memory") {
    auto system = std::move(dk::memory::MemorySystem::create().value());
    const auto heap = system.create_heap({"plan-close", dk::memory::DomainCategory::render}).value();
    CompiledGraph plan;
    {
        auto graph = take(Graph::create(heap));
        const auto image = take(graph.declare_image("image", {4, 4}));
        add(graph, "write", {image_write(image)}, true);
        plan = take(graph.compile());
        heap.begin_close();
        CHECK_FALSE(graph.compile());
    }
    CHECK(plan.resources()[0].name == "image");
    CHECK(names(plan) == std::vector<std::string_view>{"write"});
    CHECK_FALSE(heap.try_close().closed());
    plan = CompiledGraph{};
    CHECK(heap.snapshot().live_allocations == 0);
    CHECK(heap.try_close().closed());
}

TEST_CASE("graph compile partial allocation failures preserve graph and prior plans") {
    Fixture f{32768};
    const auto b = take(f.graph.declare_buffer(std::string(100, 'r'), {64}));
    add(f.graph, "producer", {write(b)});
    add(f.graph, "observer", {read(b)}, true);
    auto prior = take(f.graph.compile());
    const auto counts = f.graph.counts();
    struct Block { void* pointer; std::size_t size; };
    std::vector<Block> padding;
    while (auto block = f.heap.try_allocate(64)) padding.push_back({*block, 64});
    while (auto block = f.heap.try_allocate(1)) padding.push_back({*block, 1});
    bool failed = false, partial_failure = false, succeeded = false;
    do {
        const auto before = f.heap.snapshot();
        {
            const auto result = f.graph.compile();
            if (!result) {
                failed = true;
                partial_failure |= f.heap.snapshot().allocation_count > before.allocation_count + 3;
                CHECK(result.error().code == dk::ErrorCode::internal_error);
            } else { succeeded = true; CHECK(names(*result) == names(prior)); consistent(*result); }
        }
        CHECK(f.graph.counts() == counts);
        CHECK(f.heap.snapshot().live_allocations == before.live_allocations);
        CHECK(f.heap.snapshot().backing_requested_bytes == before.backing_requested_bytes);
        CHECK(prior.resources()[0].name == std::string(100, 'r'));
        if (padding.empty()) break;
        const auto block = padding.back(); padding.pop_back();
        f.heap.deallocate(block.pointer, block.size);
    } while (true);
    CHECK(failed); CHECK(partial_failure); CHECK(succeeded);
    CHECK(names(prior) == std::vector<std::string_view>{"producer", "observer"});
}

TEST_CASE("graph dense preserving passes retain unique hazards and deterministic order", "[graphics][graph]") {
    Fixture f;
    constexpr std::size_t count=256;
    const auto image=take(f.graph.declare_image("iterative state",{4,4,vk::Format::eR8G8B8A8Unorm,vk::ImageUsageFlagBits::eStorage,2},Lifetime::external,true));
    // Two disjoint mips generate the same logical resource edge; both hazard and content edges must deduplicate.
    for (std::size_t i=0;i<count;++i) {
        const auto access=vk::AccessFlagBits2::eShaderStorageRead|vk::AccessFlagBits2::eShaderStorageWrite;
        const std::array uses{Use{image,{{vk::PipelineStageFlagBits2::eComputeShader,access,vk::ImageLayout::eGeneral},0,VK_WHOLE_SIZE,{vk::ImageAspectFlagBits::eColor,0,1,0,1}}},
            Use{image,{{vk::PipelineStageFlagBits2::eComputeShader,access,vk::ImageLayout::eGeneral},0,VK_WHOLE_SIZE,{vk::ImageAspectFlagBits::eColor,1,1,0,1}}}};
        take(f.graph.add_pass({"iteration "+std::to_string(i),uses}));
    }
    REQUIRE(f.graph.mark_output(image));
    const auto plan=take(f.graph.compile());
    REQUIRE(plan.order().size()==count);
    for (std::size_t i=0;i<count;++i) CHECK(plan.order()[i]==i);
    CHECK(plan.dependencies().size()==3*count*(count-1)/2+count-1);
    CHECK(std::adjacent_find(plan.dependencies().begin(),plan.dependencies().end())==plan.dependencies().end());
    consistent(plan);
}

TEST_CASE("Graph cancelled compilation preserves its declarations and prior immutable plan") {
    Fixture f;
    add(f.graph,"side effect",{},true);
    const auto prior=take(f.graph.compile());
    const auto original=names(prior);
    std::stop_source stop; stop.request_stop();
    auto cancelled=f.graph.compile(stop.get_token());
    REQUIRE_FALSE(cancelled);
    REQUIRE(cancelled.error().context==std::vector<std::string>{"graph.compile.cancelled"});
    REQUIRE(names(prior)==original);
    REQUIRE(names(take(f.graph.compile()))==original);
}
