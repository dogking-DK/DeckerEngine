#include "ResourcePolicy.hpp"
#include <dk/graphics/ResourceValidation.hpp>
#include "PipelinePolicy.hpp"
#include "CommandInternal.hpp"
#include <dk/memory/MemorySystem.hpp>
#include <catch2/catch_test_macros.hpp>

using namespace dk::graphics;
namespace policy = dk::graphics::detail;
TEST_CASE("GPU timestamp conversion masks valid bits and handles wrap")
{
    REQUIRE(policy::timestamp_delta(250, 3, 8) == 9);
    REQUIRE(policy::timestamp_delta(0xfffa, 0xff03, 8) == 9);
    REQUIRE(policy::timestamp_delta(UINT64_MAX-4, 7, 64) == 12);
    REQUIRE(policy::timestamp_delta(123, 123, 36) == 0);
    const GpuProfile disabled;
    REQUIRE(disabled.status() == GpuProfileStatus::disabled);
    REQUIRE(disabled.timings().empty());
}
TEST_CASE("buffer descriptions reject unsupported features and invalid roles")
{
    REQUIRE_FALSE(policy::validate_buffer({}));
    BufferDesc desc{64};
    REQUIRE(policy::validate_buffer(desc));
    desc.usage = vk::BufferUsageFlagBits::eShaderDeviceAddress;
    REQUIRE(policy::validate_buffer(desc)); // Device enablement checked by the factory.
    REQUIRE_FALSE(Buffer{}.device_address());
    desc.usage = vk::BufferUsageFlagBits::eAccelerationStructureStorageKHR;
    REQUIRE(policy::validate_buffer(desc));
    desc.usage = vk::BufferUsageFlagBits::eAccelerationStructureBuildInputReadOnlyKHR;
    REQUIRE(policy::validate_buffer(desc));
    desc.usage = vk::BufferUsageFlagBits::eShaderBindingTableKHR;
    REQUIRE(policy::validate_buffer(desc));
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
    REQUIRE_FALSE(buffer.share());
    REQUIRE_FALSE(image.share());
    REQUIRE_FALSE(batch.state(buffer));
    REQUIRE_FALSE(batch.state(image));
    REQUIRE_FALSE(batch.finish_pass());
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
    a[0].type = b[0].type = ShaderDescriptorType::sampled_image;
    a[0].image_shape = ShaderImageShape::image_2d;
    b[0].image_shape = ShaderImageShape::image_3d;
    REQUIRE_FALSE(policy::merge_interfaces(heap, views, limits));
    a[0].image_shape = ShaderImageShape::image_3d;
    REQUIRE(policy::merge_interfaces(heap, views, limits));
    a[0].type = b[0].type = ShaderDescriptorType::uniform_buffer;
    a[0].image_shape = b[0].image_shape = ShaderImageShape::none;
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

TEST_CASE("pure access validation normalizes ranges and rejects unknown raw flag bits")
{
    const BufferDesc buffer{64};
    AccessDescription use{{vk::PipelineStageFlagBits2::eCopy, vk::AccessFlagBits2::eTransferRead}, 16};
    const auto normalized = validate_buffer_access(buffer, use);
    REQUIRE(normalized);
    CHECK(normalized->offset == 16);
    CHECK(normalized->size == 48);
    CHECK(use.size == VK_WHOLE_SIZE);
    use.state.access = vk::AccessFlags2{1ull << 63};
    CHECK_FALSE(validate_buffer_access(buffer, use));
    use.state.access = vk::AccessFlagBits2::eTransferRead;
    use.state.stages = vk::PipelineStageFlags2{1ull << 63};
    CHECK_FALSE(validate_buffer_access(buffer, use));
    use.state.stages = vk::PipelineStageFlagBits2::eCopy;
    use.state.layout = vk::ImageLayout::eTransferSrcOptimal;
    CHECK(validate_image_access({4, 4}, use));
    use.state.access = vk::AccessFlags2{1ull << 63};
    CHECK_FALSE(validate_image_access({4, 4}, use));
}

TEST_CASE("HDR color footprint uses sixteen byte texels and depth clear usage is explicit") {
    ImageDesc hdr{3,2,vk::Format::eR32G32B32A32Sfloat};
    CHECK(color_texel_bytes(hdr.format)==16); CHECK(color_texel_bytes(vk::Format::eD32Sfloat)==0);
    REQUIRE(policy::validate_image(hdr)); CHECK(*policy::image_bytes(hdr)==96);
    auto footprint=policy::copy_footprint(hdr,{0,0,0,0,3,2,16,4}); REQUIRE(footprint); CHECK(footprint->bytes==112);
    CHECK_FALSE(policy::copy_footprint(hdr,{0,0,0,0,3,2,4}));
    CHECK_FALSE(policy::copy_footprint(hdr,{0,0,0,0,3,2,0,UINT32_MAX}));
    hdr.width=hdr.height=UINT32_MAX; CHECK_FALSE(policy::image_bytes(hdr));
    ImageDesc depth{4,4,vk::Format::eD32Sfloat,vk::ImageUsageFlagBits::eDepthStencilAttachment | vk::ImageUsageFlagBits::eTransferDst};
    CHECK(policy::validate_image(depth)); CHECK_FALSE(policy::copy_footprint(depth,{0,0,0,0,4,4}));
    depth.usage|=vk::ImageUsageFlagBits::eTransferSrc; CHECK_FALSE(policy::validate_image(depth));
    CommandBatch batch; CHECK_FALSE(batch.clear_depth({},1,{vk::ImageAspectFlagBits::eDepth,0,1,0,1}));
}

TEST_CASE("volume descriptions and copies cover depth pitch mip and overflow") {
    ImageDesc volume{8,4,vk::Format::eR16G16B16A16Sfloat};
    volume.type=vk::ImageType::e3D; volume.depth=16; volume.mip_levels=5;
    REQUIRE(validate_image_description(volume)); CHECK(*policy::image_bytes(volume)==4096);
    ImageCopyRegion region{1,0,0,0,4,2,16,6,3,0,8};
    auto footprint=policy::copy_footprint(volume,region); REQUIRE(footprint);
    CHECK(footprint->full); CHECK(footprint->bytes==1088); // 7*144 + 48 + 32.
    CHECK(*image_copy_bytes(volume,region)==1088);
    CHECK(footprint->native.imageExtent.depth==8);
    region.z=1; region.depth=7;
    footprint=policy::copy_footprint(volume,region); REQUIRE(footprint); CHECK_FALSE(footprint->full);
    region.depth=8; CHECK_FALSE(image_copy_bytes(volume,region));
    region.z=0; region.layer=1; CHECK_FALSE(image_copy_bytes(volume,region));
    region.layer=0; region.depth=0; CHECK_FALSE(image_copy_bytes(volume,region));
    region.depth=8; region.image_height=1; CHECK_FALSE(image_copy_bytes(volume,region));
    region.image_height=3; region.buffer_offset=UINT64_MAX-7; CHECK_FALSE(image_copy_bytes(volume,region));
    region.buffer_offset=4; CHECK_FALSE(image_copy_bytes(volume,region));
    volume.array_layers=2; CHECK_FALSE(validate_image_description(volume)); volume.array_layers=1;
    volume.depth=0; CHECK_FALSE(validate_image_description(volume)); volume.depth=16;
    volume.type=vk::ImageType::e2D; CHECK_FALSE(validate_image_description(volume)); volume.type=vk::ImageType::e3D;
    volume.usage|=vk::ImageUsageFlagBits::eColorAttachment; CHECK_FALSE(validate_image_description(volume));
    volume.usage=vk::ImageUsageFlagBits::eStorage; volume.require_linear_filter=true; CHECK_FALSE(validate_image_description(volume));
    volume.require_linear_filter=false; volume.format=vk::Format::eD32Sfloat; CHECK_FALSE(validate_image_description(volume));
    volume.format=vk::Format::eR16G16B16A16Sfloat; volume.width=volume.height=volume.depth=UINT32_MAX;
    CHECK_FALSE(policy::image_bytes(volume));
    volume.width=volume.height=1; volume.depth=UINT32_MAX; volume.mip_levels=32;
    CHECK_FALSE(image_copy_bytes(volume,{0,0,0,0,1,1,0,268435455,UINT32_MAX,0,UINT32_MAX}));
    volume.depth=1; volume.mip_levels=1; REQUIRE(validate_image_description(volume));
    CHECK(image_copy_bytes(volume,{0,0,0,0,1,1})); // A depth-one volume is still 3D.
}
TEST_CASE("cloud UNORM footprints use one and two byte texels across padded slices") {
    for(auto format:{vk::Format::eR8Unorm,vk::Format::eR8G8Unorm}) {
        const unsigned stride=format==vk::Format::eR8Unorm ? 1u : 2u;
        CHECK(color_texel_bytes(format)==stride);
        ImageDesc image{3,5,format}; image.depth=7; image.type=vk::ImageType::e3D; image.require_linear_filter=true;
        REQUIRE(validate_image_description(image)); CHECK(*policy::image_bytes(image)==105*stride);
        CHECK(*image_copy_bytes(image,{0,0,0,0,3,5,0,0,0,0,7})==105*stride);
        auto padded=policy::copy_footprint(image,{0,0,0,0,3,5,4,6,8,0,7});
        REQUIRE(padded); CHECK(padded->bytes==(6*48+4*6+3)*stride);
        if(stride==2) CHECK_FALSE(image_copy_bytes(image,{0,0,0,0,3,5,1}));
        CHECK_FALSE(image_copy_bytes(image,{0,0,0,0,3,5,0,2}));
        image.width=image.height=image.depth=UINT32_MAX; CHECK_FALSE(policy::image_bytes(image));
    }
}
