#include "OffscreenPolicy.hpp"
#include <dk/memory/MemorySystem.hpp>
#include <catch2/catch_test_macros.hpp>
#include <limits>

using namespace dk;
using namespace dk::graphics;
namespace policy = dk::graphics::detail;
namespace {
struct Fixture {
    memory::MemorySystem system = std::move(memory::MemorySystem::create().value());
    memory::ResourceHandle resource = system.create_heap({"offscreen-policy", memory::DomainCategory::render}).value();
    CompiledShader vertex{resource}, fragment{resource}, compute{resource};
    vk::PhysicalDeviceLimits limits{};
    std::array<std::uint32_t, 4> data{0, 1, 2, 3};
    std::array<ComputeBufferInput, 2> buffers{{{0, std::as_bytes(std::span{data})}, {1, std::as_bytes(std::span{data})}}};
    Fixture()
    {
        auto artifact = [&](CompiledShader& shader, ShaderStage stage, std::uint32_t model) {
            shader.stage = stage;
            shader.entry = "main";
            // Small instruction stream for policy tests, never submitted to a driver.
            shader.spirv = {0x07230203U, 0x00010500U, 0, 2, 0, (2U << 16) | 17U, 1,
                (5U << 16) | 15U, model, 1, 0x6e69616dU, 0};
        };
        artifact(vertex, ShaderStage::vertex, 0);
        artifact(fragment, ShaderStage::fragment, 4);
        artifact(compute, ShaderStage::compute, 5);
        compute.thread_group_size = {64, 1, 1};
        for (std::uint32_t i = 0; i < 2; ++i)
            compute.bindings.push_back({String{"buffer", memory::Allocator<char>{resource}}, 0, i, ShaderDescriptorType::storage_buffer, 1, 0});
        compute.push_constants.push_back({0, 16});
        limits.maxImageDimension2D = limits.maxFramebufferWidth = limits.maxFramebufferHeight = 4096;
        limits.maxViewportDimensions[0] = limits.maxViewportDimensions[1] = 4096;
        limits.viewportBoundsRange[1] = 4096;
        for (std::size_t i = 0; i < 3; ++i) { limits.maxComputeWorkGroupCount[i] = 65535; limits.maxComputeWorkGroupSize[i] = 1024; }
        limits.maxComputeWorkGroupInvocations = 1024;
        limits.maxStorageBufferRange = 1U << 20;
        limits.maxBoundDescriptorSets = 4;
        limits.maxDescriptorSetStorageBuffers = limits.maxPerStageDescriptorStorageBuffers = limits.maxPerStageResources = 16;
        limits.maxPushConstantsSize = 128;
    }
    Result<void> dispatch(std::array<std::uint32_t, 3> groups = {1, 1, 1})
    { return policy::validate_dispatch(compute, buffers, std::as_bytes(std::span{data}), groups, limits); }
};
}
TEST_CASE("offscreen rejects malformed mismatched and optional-feature shader artifacts")
{
    Fixture f;
    REQUIRE(policy::validate_shader(f.compute, ShaderStage::compute));
    CHECK_FALSE(policy::validate_shader(f.compute, ShaderStage::fragment));
    SECTION("bad magic") { f.compute.spirv[0] = 0; }
    SECTION("missing entry") { f.compute.spirv.resize(7); }
    SECTION("wrong entry") { f.compute.entry = "other"; }
    SECTION("wrong execution model") { f.compute.spirv[8] = 0; }
    SECTION("zero instruction") { f.compute.spirv[5] = 17; }
    SECTION("truncated instruction") { f.compute.spirv[7] = (99U << 16) | 15U; }
    SECTION("optional capability") { f.compute.spirv[6] = 11; }
    CHECK_FALSE(policy::validate_shader(f.compute, ShaderStage::compute));
}
TEST_CASE("offscreen draw validates image viewport triangles and shader layout")
{
    Fixture f;
    auto size = policy::validate_draw(f.vertex, f.fragment, {}, f.limits);
    REQUIRE(size);
    CHECK(*size == 64U * 64U * 4U);
    OffscreenDraw draw;
    SECTION("zero extent") { draw.width = 0; }
    SECTION("framebuffer limit") { f.limits.maxFramebufferHeight = 32; }
    SECTION("viewport bounds") { f.limits.viewportBoundsRange[1] = 32; }
    SECTION("zero vertices") { draw.vertex_count = 0; }
    SECTION("partial triangle") { draw.vertex_count = 4; }
    SECTION("non-finite clear") { draw.clear_color[0] = std::numeric_limits<float>::quiet_NaN(); }
    SECTION("descriptor layout") { f.vertex.bindings = f.compute.bindings; }
    SECTION("push layout") { f.fragment.push_constants.push_back({0, 16}); }
    CHECK_FALSE(policy::validate_draw(f.vertex, f.fragment, draw, f.limits));
}
TEST_CASE("offscreen dispatch validates reflection inputs push constants and limits")
{
    Fixture f;
    REQUIRE(f.dispatch());
    CHECK_FALSE(f.dispatch({0, 1, 1}));
    CHECK_FALSE(f.dispatch({65536, 1, 1}));
    SECTION("duplicate input") { f.buffers[1].binding = 0; }
    SECTION("missing input") { f.buffers[1].binding = 9; }
    SECTION("empty input") { f.buffers[0].bytes = {}; }
    SECTION("unaligned data") { f.buffers[0].bytes = f.buffers[0].bytes.first(3); }
    SECTION("storage size limit") { f.limits.maxStorageBufferRange = 4; }
    SECTION("descriptor limit") { f.limits.maxPerStageResources = 1; }
    SECTION("descriptor array") { f.compute.bindings[0].count = 2; }
    SECTION("descriptor set") { f.compute.bindings[0].set = 1; }
    SECTION("descriptor type") { f.compute.bindings[0].type = ShaderDescriptorType::sampled_image; }
    SECTION("duplicate reflection") { f.compute.bindings[1].binding = 0; }
    SECTION("push size") { f.compute.push_constants[0].size = 12; }
    SECTION("push limit") { f.limits.maxPushConstantsSize = 4; }
    SECTION("push offset") { f.compute.push_constants[0].offset = 4; }
    SECTION("invocation overflow") { f.compute.thread_group_size = {1024, 1024, 1024}; }
    SECTION("zero local size") { f.compute.thread_group_size[1] = 0; }
    CHECK_FALSE(f.dispatch());
}
