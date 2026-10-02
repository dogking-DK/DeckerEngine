#include "PresentationPolicy.hpp"
#include <catch2/catch_test_macros.hpp>
using namespace dk::graphics;
namespace {
vk::SurfaceCapabilitiesKHR capabilities() {
    vk::SurfaceCapabilitiesKHR caps;
    caps.minImageCount = 2; caps.maxImageCount = 4; caps.maxImageArrayLayers = 1;
    caps.currentExtent = vk::Extent2D{UINT32_MAX, UINT32_MAX};
    caps.minImageExtent = vk::Extent2D{32, 32}; caps.maxImageExtent = vk::Extent2D{1920, 1080};
    caps.supportedUsageFlags = vk::ImageUsageFlagBits::eColorAttachment;
    caps.supportedCompositeAlpha = vk::CompositeAlphaFlagBitsKHR::eOpaque;
    return caps;
}
const std::array formats{vk::SurfaceFormatKHR{vk::Format::eR8G8B8A8Unorm, vk::ColorSpaceKHR::eSrgbNonlinear},
    vk::SurfaceFormatKHR{vk::Format::eB8G8R8A8Srgb, vk::ColorSpaceKHR::eSrgbNonlinear}};
}
TEST_CASE("swapchain policy respects capabilities and prefers sRGB") {
    auto caps = capabilities();
    auto chosen = detail::choose_swapchain(caps, formats, {3840, 1});
    REQUIRE(chosen);
    CHECK(chosen->format.format == vk::Format::eB8G8R8A8Srgb);
    CHECK(chosen->extent == vk::Extent2D{1920,32});
    CHECK(chosen->images == 3);
    caps.maxImageCount = 2; caps.currentExtent = vk::Extent2D{600,400};
    caps.supportedCompositeAlpha = vk::CompositeAlphaFlagBitsKHR::eInherit;
    chosen = detail::choose_swapchain(caps, formats, {100,100});
    REQUIRE(chosen);
    CHECK(chosen->images == 2);
    CHECK(chosen->extent == vk::Extent2D{600,400});
    CHECK(chosen->alpha == vk::CompositeAlphaFlagBitsKHR::eInherit);
    caps.maxImageCount = 0;
    CHECK(detail::choose_swapchain(caps, formats, {100,100})->images == 3);
}
TEST_CASE("swapchain rejects unsupported formats zero extent and inconsistent caps") {
    auto caps = capabilities();
    CHECK_FALSE(detail::choose_swapchain(caps, formats, {0,100}));
    CHECK_FALSE(detail::choose_swapchain(caps, {}, {100,100}));
    const std::array hdr{vk::SurfaceFormatKHR{vk::Format::eR16G16B16A16Sfloat, vk::ColorSpaceKHR::eHdr10St2084EXT}};
    CHECK_FALSE(detail::choose_swapchain(caps, hdr, {100,100}));
    const std::array any{vk::SurfaceFormatKHR{vk::Format::eUndefined, vk::ColorSpaceKHR::eSrgbNonlinear}};
    CHECK(detail::choose_swapchain(caps, any, {100,100})->format.format == vk::Format::eB8G8R8A8Srgb);
    caps.supportedUsageFlags = vk::ImageUsageFlagBits::eTransferDst;
    CHECK_FALSE(detail::choose_swapchain(caps, formats, {100,100}));
    caps = capabilities(); caps.maxImageCount = 1;
    CHECK_FALSE(detail::choose_swapchain(caps, formats, {100,100}));
    caps = capabilities(); caps.minImageExtent.width = 2000;
    CHECK_FALSE(detail::choose_swapchain(caps, formats, {100,100}));
    caps = capabilities(); caps.supportedCompositeAlpha = {};
    CHECK_FALSE(detail::choose_swapchain(caps, formats, {100,100}));
}
TEST_CASE("present failure classification preserves enqueued waits") {
    CHECK(detail::present_enqueued(VK_SUCCESS));
    CHECK(detail::present_enqueued(VK_SUBOPTIMAL_KHR));
    CHECK(detail::present_enqueued(VK_ERROR_OUT_OF_DATE_KHR));
    CHECK(detail::present_enqueued(VK_ERROR_SURFACE_LOST_KHR));
    CHECK_FALSE(detail::present_enqueued(VK_ERROR_OUT_OF_HOST_MEMORY));
    CHECK_FALSE(detail::present_enqueued(VK_ERROR_OUT_OF_DEVICE_MEMORY));
    CHECK_FALSE(detail::present_enqueued(VK_ERROR_DEVICE_LOST));
}
TEST_CASE("presentation invalid inputs are rejected before window or loader access") {
    auto result = Presenter::create({}, {}, {}, {0});
    REQUIRE_FALSE(result);
    CHECK(result.error().code == dk::ErrorCode::invalid_argument);
    CHECK_FALSE(Presenter::create({}, {}, {}, {9}));
    CHECK_FALSE(create_present_device({}, {}));
    CHECK_FALSE(Frame{});
}
