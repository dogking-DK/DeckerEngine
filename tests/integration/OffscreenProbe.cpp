#include <dk/graphics/Offscreen.hpp>
#include <dk/memory/MemorySystem.hpp>
#include "OffscreenPolicy.hpp"
#include "SubmissionInternal.hpp"
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <stdexcept>
#if defined(_MSC_VER) && defined(_DEBUG)
#include <crtdbg.h>
#endif

using namespace dk;
using namespace dk::graphics;
namespace {
void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
template<class T> T take(Result<T>&& result)
{
    if (!result) throw std::runtime_error(result.error().message);
    return std::move(*result);
}
template<class T> T take(std::expected<T, memory::AllocationError>&& result)
{
    if (!result) throw std::runtime_error("CPU Memory allocation failed");
    return std::move(*result);
}
struct Diagnostics { std::atomic<unsigned> errors = 0, warnings = 0; };
void diagnostic(void* pointer, const Diagnostic& message) noexcept
{
    auto& data = *static_cast<Diagnostics*>(pointer);
    if (message.severity == VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) ++data.errors;
    if (message.severity == VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) ++data.warnings;
    if (message.severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
        std::fprintf(stderr, "%.*s: %.*s\n", static_cast<int>(message.name.size()), message.name.data(),
            static_cast<int>(message.message.size()), message.message.data());
}
PFN_vkQueueSubmit2 native_submit = nullptr;
PFN_vkWaitSemaphores native_wait = nullptr;
VkResult submit_error = VK_SUCCESS, wait_error = VK_SUCCESS;
bool submit_exception = false;
VKAPI_ATTR VkResult VKAPI_CALL submit_override(VkQueue queue, std::uint32_t count, const VkSubmitInfo2* info, VkFence fence)
{
    if (std::exchange(submit_exception, false)) throw std::bad_alloc{};
    if (submit_error != VK_SUCCESS) return std::exchange(submit_error, VK_SUCCESS);
    return native_submit(queue, count, info, fence);
}
VKAPI_ATTR VkResult VKAPI_CALL wait_override(VkDevice device, const VkSemaphoreWaitInfo* info, std::uint64_t timeout)
{
    if (wait_error != VK_SUCCESS) return std::exchange(wait_error, VK_SUCCESS);
    return native_wait(device, info, timeout);
}
OffscreenExecutor intercepted(memory::ResourceHandle resource, Device&& device)
{
    native_submit = reinterpret_cast<PFN_vkQueueSubmit2>(device.device_proc("vkQueueSubmit2"));
    native_wait = reinterpret_cast<PFN_vkWaitSemaphores>(device.device_proc("vkWaitSemaphores"));
    auto queue = take(graphics::detail::SubmissionAccess::create(resource, std::move(device), 1,
        {submit_override, nullptr, wait_override}));
    return take(graphics::detail::OffscreenAccess::create(resource, std::move(queue)));
}
std::uint32_t allocations(const OffscreenExecutor& executor)
{
    VmaTotalStatistics stats{};
    vmaCalculateStatistics(executor.device().allocator(), &stats);
    return stats.total.statistics.allocationCount;
}
void verify_image(const OffscreenImage& image)
{
    require(image.rgba8.size() == std::size_t{image.width} * image.height * 4, "RGBA readback size mismatch");
    std::size_t inside = 0, outside = 0;
    for (std::uint32_t y = 0; y < image.height; ++y) for (std::uint32_t x = 0; x < image.width; ++x) {
        const auto index = (std::size_t{y} * image.width + x) * 4;
        const double px = 2 * (x + 0.5) / image.width - 1;
        const double py = 2 * (y + 0.5) / image.height - 1;
        const double red = 0.5 - py, green = (0.5 + py + 2 * px) / 2, blue = 1 - red - green;
        require(std::to_integer<unsigned>(image.rgba8[index + 3]) == 255, "rendered alpha mismatch");
        if (red > 0.035 && green > 0.035 && blue > 0.035) {
            const std::array expected{red * 255, green * 255, blue * 255};
            for (std::size_t c = 0; c < 3; ++c)
                require(std::abs(std::to_integer<int>(image.rgba8[index + c]) - expected[c]) <= 2.0, "triangle interpolation mismatch");
            ++inside;
        } else if (red < -0.035 || green < -0.035 || blue < -0.035) {
            for (std::size_t c = 0; c < 3; ++c) require(image.rgba8[index + c] == std::byte{}, "background clear mismatch");
            ++outside;
        }
    }
    require(inside > 100 && outside > 100, "image verification did not cover interior and background");
}
void save_ppm(const OffscreenImage& image, bool validation)
{
    std::ofstream file{validation ? "offscreen-triangle-validation.ppm" : "offscreen-triangle.ppm", std::ios::binary | std::ios::trunc};
    file << "P6\n" << image.width << ' ' << image.height << "\n255\n";
    for (std::size_t i = 0; i < image.rgba8.size(); i += 4)
        file.write(reinterpret_cast<const char*>(image.rgba8.data() + i), 3);
    if (!file) throw std::runtime_error("cannot write offscreen PPM artifact");
}
void compute_roundtrip(OffscreenExecutor& executor, const CompiledShader& compute, unsigned round)
{
    constexpr std::array<std::uint32_t, 4> counts{1, 64, 257, 1031};
    const auto count = counts[round % counts.size()];
    std::vector<std::uint32_t> input(count + 7), initial(count + 7, 0xdeadbeefU);
    for (std::size_t i = 0; i < input.size(); ++i) input[i] = static_cast<std::uint32_t>(i * 17 + round);
    const std::array<std::uint32_t, 4> parameters{count, 3 + round, 7 + round, 0};
    // Deliberately reverse descriptor order: resolution must be by binding, not input position.
    const std::array<ComputeBufferInput, 2> inputs{{{1, std::as_bytes(std::span{initial})}, {0, std::as_bytes(std::span{input})}}};
    auto output = take(executor.dispatch(compute, inputs, std::as_bytes(std::span{parameters}), {(count + 63) / 64, 1, 1}));
    require(output.size() == 2 && output[0].binding == 1 && output[1].binding == 0, "compute output binding order mismatch");
    require(output[0].bytes.size() == initial.size() * 4, "compute readback size mismatch");
    require(std::memcmp(output[1].bytes.data(), input.data(), input.size() * 4) == 0, "read-only input changed on GPU");
    for (std::size_t i = 0; i < initial.size(); ++i) {
        std::uint32_t actual = 0;
        std::memcpy(&actual, output[0].bytes.data() + i * 4, 4);
        const auto expected = i < count ? input[i] * parameters[1] + parameters[2] : 0xdeadbeefU;
        require(actual == expected, "compute transform or bounds guard mismatch");
        require(initial[i] == 0xdeadbeefU, "caller CPU input mutated");
    }
    require(allocations(executor) == 0, "compute left VMA allocations");
}
void failure_lifetimes(OffscreenExecutor& executor, const CompiledShader& vertex, const CompiledShader& fragment,
    const CompiledShader& compute)
{
    const auto submitted = executor.stats().submitted;
    // Throw before native submit to exercise ownership rollback on a CPU exception.
    submit_exception = true;
    bool threw = false;
    try { (void)executor.draw(vertex, fragment); } catch (const std::bad_alloc&) { threw = true; }
    require(threw && executor.stats().submitted == submitted && allocations(executor) == 0,
        "pre-submit exception published or retained work");
    require(take(executor.drain()), "pre-submit exception stranded a pending ticket");
    submit_error = VK_ERROR_OUT_OF_HOST_MEMORY;
    require(!executor.draw(vertex, fragment), "injected submission failure accepted");
    require(executor.stats().submitted == submitted && allocations(executor) == 0, "failed submit published or retained resources");
    wait_error = VK_TIMEOUT;
    require(!executor.draw(vertex, fragment), "injected timeout accepted");
    require(executor.stats().pending_slots == 1 && allocations(executor) == 2, "timeout released pending resources");
    require(!executor.draw(vertex, fragment), "new operation accepted while pending");
    wait_error = VK_ERROR_OUT_OF_DEVICE_MEMORY;
    require(!executor.drain(), "injected wait error accepted");
    require(allocations(executor) == 2, "wait error released pending resources");
    wait_error = VK_TIMEOUT;
    auto pending = executor.drain(0);
    require(pending && !*pending, "drain timeout did not remain pending");
    auto moved = std::move(executor);
    require(!executor.drain(), "moved-from executor accepted drain");
    require(take(moved.drain()), "drain retry failed");
    require(moved.stats().pending_slots == 0 && allocations(moved) == 0, "drain left pending objects");
    executor = std::move(moved);
    verify_image(take(executor.draw(vertex, fragment)));
    // A pending compute also retains descriptor sets/pool/layout and its pipeline.
    const std::array<std::uint32_t, 4> parameters{1, 2, 3, 0};
    const std::array<std::uint32_t, 1> value{7};
    const auto bytes = std::as_bytes(std::span{value});
    const std::array<ComputeBufferInput, 2> inputs{{{0, bytes}, {1, bytes}}};
    wait_error = VK_TIMEOUT;
    require(!executor.dispatch(compute, inputs, std::as_bytes(std::span{parameters}), {1, 1, 1}), "compute timeout accepted");
    require(executor.stats().pending_slots == 1 && allocations(executor) == 6, "compute timeout released GPU owners");
    require(take(executor.drain()) && allocations(executor) == 0, "compute drain left GPU owners");
}
} // namespace

int main(int argc, char** argv)
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
#if defined(_MSC_VER) && defined(_DEBUG)
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
#endif
    if (argc > 2 || (argc == 2 && std::strcmp(argv[1], "--validation") != 0)) return 2;
    const bool validation = argc == 2;
    Diagnostics diagnostics;
    try {
        auto system = take(memory::MemorySystem::create());
        const auto resource = take(system.create_heap({"offscreen-probe", memory::DomainCategory::render}));
        DeviceOptions options;
        options.validation = validation ? ValidationMode::required : ValidationMode::disabled;
        options.diagnostic_sink = diagnostic;
        options.diagnostic_user_data = &diagnostics;
        // Check device availability before spending time on shader compilation.
        auto first_device = Device::create(resource, options);
        if (!first_device) {
            std::fprintf(stderr, "%s\n", first_device.error().message.c_str());
            if (first_device.error().code == ErrorCode::not_found || first_device.error().code == ErrorCode::not_supported) return 77;
            return 1;
        }
        const auto& info = first_device->adapter();
        std::printf("GPU=%s driver=%s (%s) API=%u.%u.%u validation=%s\n", info.properties.deviceName.data(),
            info.driver.driverName.data(), info.driver.driverInfo.data(), VK_API_VERSION_MAJOR(info.properties.apiVersion),
            VK_API_VERSION_MINOR(info.properties.apiVersion), VK_API_VERSION_PATCH(info.properties.apiVersion), validation ? "required" : "disabled");
        {
            const auto sources = std::filesystem::path{DK_COMMON_SHADER_DIR};
            auto vertex = take(compile_shader({sources / "triangle.slang", "vertexMain", ShaderStage::vertex}, resource));
            auto fragment = take(compile_shader({sources / "triangle.slang", "fragmentMain", ShaderStage::fragment}, resource));
            auto compute = take(compile_shader({sources / "transform.slang", "computeMain", ShaderStage::compute}, resource));
            std::printf("Slang=%s target=spirv_1_5 matrix=row_major entries=vertexMain,fragmentMain,computeMain\n", compute.compiler.c_str());
            for (unsigned round = 0; round < 3; ++round) {
                auto device = round == 0 ? std::move(*first_device) : take(Device::create(resource, options));
                auto executor = intercepted(resource, std::move(device));
                for (unsigned iteration = 0; iteration < 4; ++iteration) {
                    OffscreenDraw draw{64 + iteration * 8, 64 + iteration * 4};
                    auto image = take(executor.draw(vertex, fragment, draw));
                    verify_image(image);
                    if (round == 0 && iteration == 0) save_ppm(image, validation);
                    require(allocations(executor) == 0, "draw left VMA allocations");
                    compute_roundtrip(executor, compute, iteration);
                }
                failure_lifetimes(executor, vertex, fragment, compute);
                const auto count = executor.stats().submitted;
                OffscreenDraw invalid{};
                invalid.width = 0;
                require(!executor.draw(vertex, fragment, invalid), "invalid draw accepted");
                require(executor.stats().submitted == count && allocations(executor) == 0, "invalid draw changed GPU state");
                // Leave a timed-out draw pending: destruction must wait before child RAII teardown.
                wait_error = VK_TIMEOUT;
                require(!executor.draw(vertex, fragment), "destructor test failed to leave work pending");
            }
            {
                const auto limited = take(system.create_heap({"offscreen-closing", memory::DomainCategory::render}));
                {
                    auto executor = take(OffscreenExecutor::create(limited, take(Device::create(limited, options))));
                    limited.begin_close();
                    const auto rejected = executor.draw(vertex, fragment);
                    require(!rejected && rejected.error().code == ErrorCode::invalid_state, "closing resource accepted draw");
                    require(!executor.dispatch(compute, {}, {}, {1, 1, 1}), "closing resource accepted compute");
                    require(executor.stats().submitted == 0 && allocations(executor) == 0, "closing resource published work");
                    require(take(executor.drain()), "closing resource prevented drain");
                }
                require(limited.snapshot().live_allocations == 0, "closing executor left Memory allocations");
            }
        }
        // first_device is moved from; all real devices and shader owners were destroyed above.
        require(resource.snapshot().live_allocations == 0, "offscreen probe left Memory allocations");
        std::printf("draws=12 computes=12 lifecycle_rounds=3 pending=0 VMA=0 Memory=0 errors=%u warnings=%u\n",
            diagnostics.errors.load(), diagnostics.warnings.load());
        require(diagnostics.errors == 0 && diagnostics.warnings == 0, "offscreen validation messages occurred");
        return 0;
    } catch (const std::exception& error) { std::fprintf(stderr, "offscreen probe failed: %s\n", error.what()); return 1; }
}
