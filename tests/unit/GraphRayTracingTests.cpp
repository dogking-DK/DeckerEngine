#include <dk/graphics/Graph.hpp>
#include <dk/graphics/GraphDiagnostics.hpp>
#include <dk/memory/MemorySystem.hpp>
#include <catch2/catch_test_macros.hpp>
#include <algorithm>

using namespace dk::graphics;
using namespace dk::graphics::graph;
namespace {
struct Fixture {
    dk::memory::MemorySystem system=dk::memory::MemorySystem::create().value();
    dk::memory::ResourceHandle heap=system.create_heap({"graph-rt-tests",dk::memory::DomainCategory::render}).value();
    Graph graph=Graph::create(heap).value();
};
template<class T> T take(dk::Result<T> value) { if (!value) { INFO(value.error().message); FAIL("unexpected graph error"); } return std::move(*value); }
constexpr auto bottom=AccelerationStructureKind::bottom_level,top=AccelerationStructureKind::top_level;
constexpr auto input_usage=vk::BufferUsageFlagBits::eAccelerationStructureBuildInputReadOnlyKHR|vk::BufferUsageFlagBits::eShaderDeviceAddress|vk::BufferUsageFlagBits::eTransferDst;
AccelerationId declare(Graph& graph,std::string_view name,AccelerationStructureKind kind,std::initializer_list<ResourceId> deps,bool initialized=false) {
    return take(graph.declare_acceleration(name,kind,{deps.begin(),deps.size()},initialized));
}
PassId add(Graph& graph,std::string_view name,std::initializer_list<Use> uses,bool effect=false) {
    return take(graph.add_pass({name,{uses.begin(),uses.size()},effect}));
}
Use write(BufferId id) { return {id,{{vk::PipelineStageFlagBits2::eCopy,vk::AccessFlagBits2::eTransferWrite},0,VK_WHOLE_SIZE,{},true}}; }
}
TEST_CASE("graph RT preserves upload build trace contents and culls unused AS") {
    Fixture f; auto& g=f.graph;
    const auto input=take(g.declare_buffer("geometry",{36,input_usage},Lifetime::external));
    const auto output=take(g.declare_buffer("output",{32,vk::BufferUsageFlagBits::eStorageBuffer}));
    auto deps=std::array<ResourceId,1>{input};
    const auto blas=take(g.declare_acceleration("blas",bottom,deps));
    const auto dead=declare(g,"dead",bottom,{input});
    const auto tlas=declare(g,"tlas",top,{blas});
    const auto sbt=take(g.declare_shader_table("sbt"));
    add(g,"upload",{write(input)});
    add(g,"dead build",{build_input(input),acceleration_build(dead)});
    add(g,"BLAS build",{build_input(input),acceleration_build(blas)});
    add(g,"TLAS build",{acceleration_read(blas,false),acceleration_build(tlas)});
    add(g,"trace",{acceleration_read(blas),acceleration_read(tlas),shader_table_read(sbt),
        {output,{{vk::PipelineStageFlagBits2::eRayTracingShaderKHR,vk::AccessFlagBits2::eShaderWrite},0,VK_WHOLE_SIZE,{},true}}});
    REQUIRE(g.mark_output(output));
    auto plan=take(g.compile());
    REQUIRE(plan.order().size()==4); CHECK(plan.order()[0]==0); CHECK(plan.order()[1]==2); CHECK(plan.order()[2]==3); CHECK(plan.order()[3]==4);
    CHECK_FALSE(plan.passes()[1].retained); CHECK_FALSE(plan.resources()[3].retained);
    CHECK(plan.resources()[2].dependencies[0]==0); CHECK(plan.resources()[4].dependencies[0]==2);
    for (const auto pair : {std::array<std::size_t,2>{0,2},{2,3},{3,4}})
        CHECK(std::ranges::any_of(plan.dependencies(),[&](const auto& edge) { return edge.before==pair[0] && edge.after==pair[1] && edge.kind==DependencyKind::contents; }));
    CHECK(plan.allocations().size()==1);
    REQUIRE(g.reset()); CHECK_FALSE(blas); deps[0]=BufferId{};
    CHECK(plan.resources()[4].dependencies[0]==2);
    auto report=take(format_plan(plan)); CHECK(report.text().find("BLAS")!=std::string_view::npos); CHECK(report.text().find("SBT")!=std::string_view::npos);
}
TEST_CASE("graph RT rejects missing closure uninitialized reads repeated builds and scratch misuse") {
    Fixture f; auto& g=f.graph;
    const auto input=take(g.declare_buffer("input",{36,input_usage},Lifetime::external,true));
    const auto blas=declare(g,"blas",bottom,{input}); const auto tlas=declare(g,"tlas",top,{blas});
    const std::array incomplete{acceleration_build(blas)};
    CHECK_FALSE(g.add_pass({"missing input",incomplete}));
    const std::array missing_child{acceleration_build(tlas)}; CHECK_FALSE(g.add_pass({"missing child",missing_child}));
    add(g,"unbuilt trace",{acceleration_read(blas),acceleration_read(tlas)}); CHECK_FALSE(g.compile());
    REQUIRE(g.reset());
    const auto input2=take(g.declare_buffer("input",{36,input_usage},Lifetime::external,true));
    const auto blas2=declare(g,"blas",bottom,{input2});
    add(g,"first",{build_input(input2),acceleration_build(blas2)});
    add(g,"repeat",{build_input(input2),acceleration_build(blas2)}); CHECK_FALSE(g.compile());
    const auto scratch=take(g.declare_buffer("scratch",{256,vk::BufferUsageFlagBits::eStorageBuffer|vk::BufferUsageFlagBits::eShaderDeviceAddress}));
    auto scratch_use=build_input(scratch); CHECK_FALSE(g.add_pass({"scratch is not input",std::span{&scratch_use,1}}));
    scratch_use.access.state.access=vk::AccessFlagBits2::eAccelerationStructureReadKHR|vk::AccessFlagBits2::eAccelerationStructureWriteKHR;
    scratch_use.access.full_overwrite=true; CHECK_FALSE(g.add_pass({"scratch cannot initialize business content",std::span{&scratch_use,1}}));
}
TEST_CASE("graph RT handles imported AS outputs immutable identity and access rejection") {
    Fixture f; auto& g=f.graph;
    const auto blas=declare(g,"ready BLAS",bottom,{},true); const auto tlas=declare(g,"ready TLAS",top,{blas},true);
    REQUIRE(g.mark_output(tlas)); auto plan=take(g.compile());
    CHECK(plan.order().empty()); CHECK(plan.resources()[0].retained); CHECK(plan.resources()[1].output); CHECK(plan.allocations().empty());
    const auto count=g.counts();
    auto foreign=take(Graph::create(f.heap)); const auto other=declare(foreign,"other",bottom,{},true);
    std::array<ResourceId,1> dependencies{other}; CHECK_FALSE(g.declare_acceleration("foreign",top,dependencies,true));
    CHECK(g.counts()==count);
    const std::array bad_build{acceleration_build(blas)}; CHECK_FALSE(g.add_pass({"rebuild imported",bad_build}));
    const auto sbt=take(g.declare_shader_table("table"));
    auto use=shader_table_read(sbt); use.access.state.access=vk::AccessFlagBits2::eShaderWrite; use.access.full_overwrite=true;
    CHECK_FALSE(g.add_pass({"write SBT",std::span{&use,1}}));
    use=acceleration_read(blas); use.access.state.stages=vk::PipelineStageFlagBits2::eComputeShader;
    CHECK_FALSE(g.add_pass({"AS compute access",std::span{&use,1}}));
    CHECK_FALSE(g.mark_output(AccelerationId{})); REQUIRE(g.reset()); CHECK_FALSE(g.mark_output(blas));
}
