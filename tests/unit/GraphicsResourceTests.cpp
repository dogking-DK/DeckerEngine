#include "ResourcePolicy.hpp"
#include "PipelinePolicy.hpp"
#include "CommandInternal.hpp"
#include <dk/memory/MemorySystem.hpp>
#include <catch2/catch_test_macros.hpp>

using namespace dk::graphics;
namespace policy = dk::graphics::detail;
TEST_CASE("buffer descriptions reject unsupported features and invalid roles")
{
    REQUIRE_FALSE(policy::validate_buffer({}));
    BufferDesc desc{64};
    REQUIRE(policy::validate_buffer(desc));
    desc.usage = vk::BufferUsageFlagBits::eShaderDeviceAddress;
    REQUIRE_FALSE(policy::validate_buffer(desc));
    desc.usage = vk::BufferUsageFlags{0x80000000u};
    REQUIRE_FALSE(policy::validate_buffer(desc));
    desc.usage = vk::BufferUsageFlagBits::eTransferSrc;
    desc.memory = static_cast<BufferMemory>(99);
    REQUIRE_FALSE(policy::validate_buffer(desc));
}
TEST_CASE("image descriptions reject zero extent format and byte overflow")
{
    REQUIRE_FALSE(policy::image_bytes({}));
    ImageDesc image{16, 32};
    REQUIRE(*policy::image_bytes(image) == 2048);
    image.format = vk::Format::eD32Sfloat;
    REQUIRE_FALSE(policy::image_bytes(image));
    image.format = vk::Format::eR32Sfloat;
    image.width = image.height = std::numeric_limits<std::uint32_t>::max();
    REQUIRE_FALSE(policy::image_bytes(image));
    image.width = image.height = 1;
    image.usage = vk::ImageUsageFlagBits::eDepthStencilAttachment;
    REQUIRE_FALSE(policy::image_bytes(image));
    image.usage = vk::ImageUsageFlags{0x80000000u};
    REQUIRE_FALSE(policy::image_bytes(image));
}
TEST_CASE("buffer copy checks alignment ranges overlap and overflow")
{
    REQUIRE(policy::validate_copy(64, 64, 32, 0, 32, true));
    REQUIRE_FALSE(policy::validate_copy(64, 64, 32, 0, 28, true));
    REQUIRE(policy::validate_copy(64, 64, 32, 0, 28, false));
    REQUIRE_FALSE(policy::validate_copy(64, 64, 0, 0, 0, false));
    REQUIRE_FALSE(policy::validate_copy(64, 64, 5, 0, 0, false));
    REQUIRE_FALSE(policy::validate_copy(64, 64, 4, 2, 0, false));
    REQUIRE_FALSE(policy::validate_copy(64, 64, 4, 64, 0, false));
    constexpr auto maximum = std::numeric_limits<vk::DeviceSize>::max();
    REQUIRE_FALSE(policy::valid_range(maximum, maximum - 1, 4));
    REQUIRE(policy::valid_range(64, 64, 0));
}
TEST_CASE("image layouts require matching usages and never discard implicitly")
{
    const auto usage = vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eSampled;
    REQUIRE(policy::validate_layout(vk::ImageLayout::eTransferDstOptimal, usage));
    REQUIRE(policy::validate_layout(vk::ImageLayout::eShaderReadOnlyOptimal, usage));
    REQUIRE(policy::validate_layout(vk::ImageLayout::eGeneral, usage));
    REQUIRE_FALSE(policy::validate_layout(vk::ImageLayout::eUndefined, usage));
    REQUIRE_FALSE(policy::validate_layout(vk::ImageLayout::eTransferSrcOptimal, usage));
    REQUIRE_FALSE(policy::validate_layout(vk::ImageLayout::eColorAttachmentOptimal, usage));
    REQUIRE_FALSE(policy::validate_layout(vk::ImageLayout::ePresentSrcKHR, usage));
}
TEST_CASE("empty resource and batch operations reject without native calls")
{
    Buffer buffer;
    Image image;
    CommandBatch batch;
    REQUIRE_FALSE(buffer);
    REQUIRE_FALSE(image);
    REQUIRE_FALSE(buffer.write(0, {}));
    REQUIRE_FALSE(buffer.read(0, {}));
    REQUIRE_FALSE(buffer.state());
    REQUIRE_FALSE(batch.retain(buffer));
    REQUIRE_FALSE(batch.retain(image));
    REQUIRE_FALSE(batch.copy(buffer, buffer, 4));
    REQUIRE_FALSE(batch.compute());
    REQUIRE_FALSE(batch.unsafe_record({}, {}, [](const vk::raii::CommandBuffer&,void*) {}));
}
TEST_CASE("pipeline interfaces merge stages preserve array counts and reject conflicts")
{
    auto system = std::move(dk::memory::MemorySystem::create().value());
    auto heap = system.create_heap({"layout-tests", dk::memory::DomainCategory::render}).value();
    vk::PhysicalDeviceLimits limits{};
    limits.maxBoundDescriptorSets = 4;
    limits.maxPushConstantsSize = 128;
    limits.maxDescriptorSetUniformBuffers = limits.maxPerStageDescriptorUniformBuffers = 16;
    limits.maxDescriptorSetStorageBuffers = limits.maxPerStageDescriptorStorageBuffers = 16;
    limits.maxDescriptorSetSampledImages = limits.maxPerStageDescriptorSampledImages = 16;
    limits.maxDescriptorSetStorageImages = limits.maxPerStageDescriptorStorageImages = 16;
    limits.maxDescriptorSetSamplers = limits.maxPerStageDescriptorSamplers = 16;
    limits.maxPerStageResources = 32;
    std::array<policy::ReflectedBinding, 1> a{{{2, 3, ShaderDescriptorType::uniform_buffer, 2, 16}}};
    std::array<policy::ReflectedBinding, 1> b{{{2, 3, ShaderDescriptorType::uniform_buffer, 2, 32}}};
    const std::array<ShaderPushConstant, 2> push_a{{{0, 4}, {16, 4}}};
    const std::array<ShaderPushConstant, 1> push_b{{{0, 20}}};
    const std::array views{policy::ShaderLayoutView{ShaderStage::vertex, a, push_a},
        policy::ShaderLayoutView{ShaderStage::fragment, b, push_b}};
    auto merged = policy::merge_interfaces(heap, views, limits);
    REQUIRE(merged);
    REQUIRE(merged->bindings.size() == 1);
    REQUIRE(merged->bindings[0].count == 2);
    REQUIRE(merged->bindings[0].minimum_buffer_size == 32);
    REQUIRE(merged->bindings[0].stages == (vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment));
    REQUIRE(merged->push.size() == 1);
    REQUIRE(merged->push[0].size == 20);
    b[0].count = 3;
    REQUIRE_FALSE(policy::merge_interfaces(heap, views, limits));
    b[0].count = 2;
    b[0].type = ShaderDescriptorType::storage_buffer;
    REQUIRE_FALSE(policy::merge_interfaces(heap, views, limits));
    b[0].type = ShaderDescriptorType::uniform_buffer;
    limits.maxPerStageDescriptorUniformBuffers = 1;
    REQUIRE_FALSE(policy::merge_interfaces(heap, views, limits));
    limits.maxPerStageDescriptorUniformBuffers = 16;
    limits.maxPushConstantsSize = 16;
    REQUIRE_FALSE(policy::merge_interfaces(heap, views, limits));
}

TEST_CASE("image subresource and pitched copy validation rejects invalid footprints")
{
    ImageDesc image{8,8,vk::Format::eR8G8B8A8Unorm,vk::ImageUsageFlagBits::eTransferDst,4,2};
    REQUIRE(policy::validate_image(image));
    REQUIRE_FALSE(policy::validate_subresources(image,{vk::ImageAspectFlagBits::eColor,4,1,0,1}));
    REQUIRE_FALSE(policy::validate_subresources(image,{vk::ImageAspectFlagBits::eDepth,0,1,0,1}));
    auto copy = policy::copy_footprint(image,{1,1,0,0,4,4,16,8,4});
    REQUIRE(copy);
    REQUIRE(copy->bytes == 112);
    REQUIRE(copy->full);
    REQUIRE_FALSE(policy::copy_footprint(image,{1,1,1,0,4,4}));
    REQUIRE_FALSE(policy::copy_footprint(image,{1,2,0,0,4,4}));
    REQUIRE_FALSE(policy::copy_footprint(image,{1,1,0,0,4,4,0,3}));
    image.mip_levels = 5;
    REQUIRE_FALSE(policy::validate_image(image));
    REQUIRE(policy::stage_covers(vk::PipelineStageFlagBits2::eAllGraphics,vk::PipelineStageFlagBits2::eVertexAttributeInput));
    REQUIRE_FALSE(policy::stage_covers(vk::PipelineStageFlagBits2::eAllGraphics,vk::PipelineStageFlagBits2::eComputeShader));
    REQUIRE_FALSE(policy::stage_covers(vk::PipelineStageFlagBits2::eAllGraphics,vk::PipelineStageFlagBits2::eCopy));
}
