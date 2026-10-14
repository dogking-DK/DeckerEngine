#include "RayTracingInternal.hpp"
#include "PipelinePolicy.hpp"
#include <dk/memory/MemorySystem.hpp>
#include <catch2/catch_test_macros.hpp>

using namespace dk::graphics;
namespace policy=dk::graphics::detail;
TEST_CASE("SBT regions satisfy independent handle base alignment and bounds") {
    vk::PhysicalDeviceRayTracingPipelinePropertiesKHR limits{};
    limits.shaderGroupHandleSize=24; limits.shaderGroupHandleAlignment=32; limits.shaderGroupBaseAlignment=64; limits.maxShaderGroupStride=4096;
    auto layout=policy::shader_table_layout(limits,3,5); REQUIRE(layout);
    CHECK(layout->stride==32); CHECK(layout->miss_offset==64); CHECK(layout->hit_offset==192); CHECK(layout->bytes==352); CHECK(layout->capacity==415);
    REQUIRE_FALSE(policy::shader_table_layout(limits,0,1));
    REQUIRE_FALSE(policy::shader_table_layout(limits,1,0));
    REQUIRE_FALSE(policy::shader_table_layout(limits,UINT32_MAX,1));
    limits.maxShaderGroupStride=16; REQUIRE_FALSE(policy::shader_table_layout(limits,1,1));
    limits.maxShaderGroupStride=4096; limits.shaderGroupBaseAlignment=3; REQUIRE_FALSE(policy::shader_table_layout(limits,1,1));
    limits.shaderGroupBaseAlignment=64; limits.shaderGroupHandleAlignment=0; REQUIRE_FALSE(policy::shader_table_layout(limits,1,1));
}
TEST_CASE("ray dispatch and instance hit indexing reject overflow truncation and limits") {
    vk::PhysicalDeviceLimits limits{}; limits.maxComputeWorkGroupCount=std::array<std::uint32_t,3>{1024,1024,64};
    limits.maxComputeWorkGroupSize=std::array<std::uint32_t,3>{128,128,64};
    REQUIRE(policy::ray_dispatch_dimensions({128,2,1},limits,256));
    REQUIRE_FALSE(policy::ray_dispatch_dimensions({129,2,1},limits,256));
    REQUIRE_FALSE(policy::ray_dispatch_dimensions({1,0,1},limits,256));
    REQUIRE_FALSE(policy::ray_dispatch_dimensions({1,1,4097},limits,UINT32_MAX));
    REQUIRE_FALSE(policy::ray_dispatch_dimensions({UINT32_MAX,UINT32_MAX,UINT32_MAX},limits,UINT32_MAX));
    REQUIRE(policy::ray_hit_range(4,2,2,8));
    REQUIRE_FALSE(policy::ray_hit_range(5,2,2,8));
    REQUIRE_FALSE(policy::ray_hit_range(UINT32_MAX,UINT32_MAX,15,UINT32_MAX));
    REQUIRE_FALSE(policy::ray_hit_range(0,1,16,100));
    REQUIRE_FALSE(policy::ray_hit_range(0,0,2,8));
}
TEST_CASE("ray payload compatibility compares structural types not just byte sizes") {
    auto system=dk::memory::MemorySystem::create().value(); auto heap=system.create_heap({"rt-policy",dk::memory::DomainCategory::render}).value();
    ShaderRayInterface ray{heap},hit{heap};
    ray.payloads.push_back({RayInterfaceKind::outgoing_payload,2,8,dk::String{"s(f32;u32;)",dk::memory::Allocator<char>{heap}}});
    hit.payloads.push_back({RayInterfaceKind::incoming_payload,0,8,dk::String{"s(f32;u32;)",dk::memory::Allocator<char>{heap}}});
    const ShaderRayTrace trace{0,2,0,2};
    REQUIRE(policy::ray_payload_compatible(ray,hit,trace));
    hit.payloads[0].signature="s(u32;f32;)"; REQUIRE_FALSE(policy::ray_payload_compatible(ray,hit,trace));
    hit.payloads[0].signature="s(f32;u32;)";
    hit.payloads.push_back({RayInterfaceKind::hit_attribute,0,8,dk::String{"v2(u32)",dk::memory::Allocator<char>{heap}}});
    REQUIRE_FALSE(policy::ray_payload_compatible(ray,hit,trace));
    hit.payloads.back().signature="v2(f32)"; REQUIRE(policy::ray_payload_compatible(ray,hit,trace));
    REQUIRE_FALSE(policy::ray_payload_compatible(ray,hit,{0,2,0,3}));
}
TEST_CASE("RT layout merges repeated stages and enforces acceleration descriptor limits") {
    auto system=dk::memory::MemorySystem::create().value(); auto heap=system.create_heap({"rt-layout",dk::memory::DomainCategory::render}).value();
    vk::PhysicalDeviceLimits limits{}; limits.maxBoundDescriptorSets=4; limits.maxPerStageResources=16;
    vk::PhysicalDeviceAccelerationStructurePropertiesKHR as{}; as.maxDescriptorSetAccelerationStructures=2; as.maxPerStageDescriptorAccelerationStructures=2;
    const std::array bindings{policy::ReflectedBinding{0,0,ShaderDescriptorType::acceleration_structure,2,0}};
    std::array views{policy::ShaderLayoutView{ShaderStage::miss,bindings,{}},policy::ShaderLayoutView{ShaderStage::miss,bindings,{}}};
    auto merged=policy::merge_interfaces(heap,views,limits,as); REQUIRE(merged); CHECK(merged->bindings.size()==1);
    as.maxPerStageDescriptorAccelerationStructures=1; REQUIRE_FALSE(policy::merge_interfaces(heap,views,limits,as));
    as.maxPerStageDescriptorAccelerationStructures=2; as.maxDescriptorSetAccelerationStructures=1; REQUIRE_FALSE(policy::merge_interfaces(heap,views,limits,as));
    views[1].stage=ShaderStage::compute; REQUIRE_FALSE(policy::merge_interfaces(heap,views,limits,as));
}

TEST_CASE("RT texture arrays enforce sampler image set stage and aggregate limits") {
    auto system=dk::memory::MemorySystem::create().value(); auto heap=system.create_heap({"texture-layout",dk::memory::DomainCategory::render}).value();
    vk::PhysicalDeviceLimits limits{}; limits.maxBoundDescriptorSets=1; limits.maxPerStageResources=64;
    limits.maxDescriptorSetSampledImages=limits.maxPerStageDescriptorSampledImages=64;
    limits.maxDescriptorSetSamplers=limits.maxPerStageDescriptorSamplers=64;
    const std::array bindings{policy::ReflectedBinding{0,0,ShaderDescriptorType::sampled_image,64,0,ShaderImageShape::image_2d},
        policy::ReflectedBinding{0,1,ShaderDescriptorType::sampler,64,0}};
    const std::array views{policy::ShaderLayoutView{ShaderStage::raygeneration,bindings,{}},policy::ShaderLayoutView{ShaderStage::anyhit,bindings,{}}};
    REQUIRE(policy::merge_interfaces(heap,views,limits));
    for(int which=0;which<5;++which) {
        auto low=limits;
        if(which==0) low.maxDescriptorSetSampledImages=63;
        if(which==1) low.maxPerStageDescriptorSampledImages=63;
        if(which==2) low.maxDescriptorSetSamplers=63;
        if(which==3) low.maxPerStageDescriptorSamplers=63;
        if(which==4) low.maxPerStageResources=63;
        CHECK_FALSE(policy::merge_interfaces(heap,views,low));
    }
}
