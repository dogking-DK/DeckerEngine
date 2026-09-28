#include "ResourcePolicy.hpp"
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
    REQUIRE_FALSE(batch.retain(buffer));
    REQUIRE_FALSE(batch.retain(image));
    REQUIRE_FALSE(batch.copy(buffer, buffer, 4));
    REQUIRE_THROWS_AS(batch.command_buffer(), std::logic_error);
}
