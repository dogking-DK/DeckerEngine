#include <dk/graphics/Resources.hpp>
#include <dk/memory/MemorySystem.hpp>
#include "SubmissionInternal.hpp"
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <stdexcept>

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
void check(Result<void> result) { if (!result) throw std::runtime_error(result.error().message); }
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
VkResult submit_error = VK_SUCCESS;
VkResult wait_error = VK_SUCCESS;
bool timeout_once = false;
VKAPI_ATTR VkResult VKAPI_CALL submit_override(VkQueue queue, std::uint32_t count, const VkSubmitInfo2* info, VkFence fence)
{
    if (submit_error != VK_SUCCESS) return std::exchange(submit_error, VK_SUCCESS);
    return native_submit(queue, count, info, fence);
}
VKAPI_ATTR VkResult VKAPI_CALL wait_override(VkDevice device, const VkSemaphoreWaitInfo* info, std::uint64_t timeout)
{
    if (wait_error != VK_SUCCESS) return std::exchange(wait_error, VK_SUCCESS);
    if (std::exchange(timeout_once, false)) return VK_TIMEOUT;
    return native_wait(device, info, timeout);
}
SubmissionQueue queue(memory::ResourceHandle resource, const DeviceOptions& options, bool intercept = false)
{
    auto device = take(Device::create(resource, options));
    if (!intercept) return take(SubmissionQueue::create(resource, std::move(device), 2));
    native_submit = reinterpret_cast<PFN_vkQueueSubmit2>(device.device_proc("vkQueueSubmit2"));
    native_wait = reinterpret_cast<PFN_vkWaitSemaphores>(device.device_proc("vkWaitSemaphores"));
    return take(graphics::detail::SubmissionAccess::create(resource, std::move(device), 2,
        {submit_override, nullptr, wait_override}));
}
Buffer buffer(SubmissionQueue& queue, std::size_t size, BufferMemory role)
{ return take(queue.create_buffer({size, vk::BufferUsageFlagBits::eTransferSrc | vk::BufferUsageFlagBits::eTransferDst, role})); }
std::uint32_t allocations(const SubmissionQueue& queue)
{
    VmaTotalStatistics stats{};
    vmaCalculateStatistics(queue.device().allocator(), &stats);
    return stats.total.statistics.allocationCount;
}
void roundtrip(SubmissionQueue& queue, unsigned round)
{
    std::array<std::byte, 1024> input{}, actual{};
    for (std::size_t i = 0; i < input.size(); ++i) input[i] = static_cast<std::byte>((i * 37 + round * 13) & 255);
    auto upload = buffer(queue, input.size(), BufferMemory::upload);
    auto gpu = buffer(queue, input.size(), BufferMemory::device);
    auto output = buffer(queue, input.size(), BufferMemory::readback);
    auto image_output = buffer(queue, input.size(), BufferMemory::readback);
    constexpr std::array formats{vk::Format::eR8G8B8A8Unorm, vk::Format::eR8G8B8A8Srgb,
        vk::Format::eB8G8R8A8Unorm, vk::Format::eR32Uint, vk::Format::eR32Sfloat};
    auto image = take(queue.create_image({16, 16, formats[round % formats.size()]}));
    check(upload.write(0, input));
    check(upload.write(input.size(), {}));
    require(!upload.write(input.size(), input), "out of range host write accepted");
    require(!gpu.write(0, input), "device-only host write accepted");
    {
        auto batch = take(queue.begin());
        check(batch.transition(image, vk::ImageLayout::eTransferDstOptimal));
        check(batch.copy(upload, gpu, input.size()));
        require(!queue.close(), "close accepted active recording");
        require(!upload.write(0, input), "recording data overwritten");
        auto moved = std::move(batch);
        require(!batch.retain(gpu), "moved batch remained valid");
        auto competing = take(queue.begin());
        require(!competing.retain(image), "two image recording reservations accepted");
    }
    require(image.layout() == vk::ImageLayout::eUndefined, "abandon published an image layout");
    require(queue.stats().free_slots == 2, "abandoned slots were not released");
    check(upload.write(0, input));
    {
        auto rejected = take(queue.begin());
        check(rejected.copy_to_image(upload, image));
        submit_error = VK_ERROR_OUT_OF_HOST_MEMORY;
        const auto before = queue.stats().submitted;
        const auto result = queue.submit(std::move(rejected));
        require(!result && result.error().message.find("VK_ERROR_OUT_OF_HOST_MEMORY") != std::string::npos, "submit failure not reported");
        require(queue.stats().submitted == before && queue.stats().free_slots == 2, "failed submit published work");
        require(image.layout() == vk::ImageLayout::eUndefined, "failed submit published layout");
        check(upload.write(0, input));
    }
    auto first = take(queue.begin());
    check(first.copy(upload, gpu, input.size()));
    check(first.copy_to_image(upload, image));
    check(first.transition(image, vk::ImageLayout::eShaderReadOnlyOptimal));
    auto first_ticket = take(queue.submit(std::move(first)));
    require(image.layout() == vk::ImageLayout::eShaderReadOnlyOptimal, "successful submit did not publish layout");
    const auto count = allocations(queue);
    upload = Buffer{};
    require(allocations(queue) == count, "pending upload freed early");
    auto second = take(queue.begin());
    check(second.copy(gpu, output, input.size()));
    check(second.copy_to_buffer(image, image_output));
    auto last_ticket = take(queue.submit(std::move(second)));
    gpu = Buffer{};
    image = Image{};
    require(!queue.begin(), "pending command pool reused without collection");
    require(!output.read(0, actual), "pending readback allowed before collection");
    timeout_once = true;
    require(!take(queue.wait(last_ticket, 0)), "injected timeout did not return false");
    require(queue.stats().pending_slots == 2 && allocations(queue) == count, "timeout reclaimed pending work");
    wait_error = VK_ERROR_OUT_OF_HOST_MEMORY;
    require(!queue.wait(last_ticket, 0), "wait failure not reported");
    require(queue.stats().pending_slots == 2 && allocations(queue) == count, "wait error reclaimed pending work");
    if (round % 2 == 0) {
        // Independently establish GPU completion so poll's release path is deterministic.
        queue.device().queue().waitIdle();
        check(queue.poll());
        require(queue.stats().pending_slots == 0, "poll did not collect completed slots");
    }
    require(take(queue.wait(last_ticket)), "wait failed");
    require(take(queue.wait(first_ticket, 0)), "older completed ticket not recognized");
    require(queue.stats().pending_slots == 0 && allocations(queue) == 2, "completion did not reclaim retained resources");
    check(output.read(0, actual));
    require(actual == input, "buffer roundtrip mismatch");
    actual.fill(std::byte{});
    check(image_output.read(0, actual));
    require(actual == input, "image roundtrip mismatch");
    check(queue.poll());
}
void cross_queue(SubmissionQueue& first, SubmissionQueue& second)
{
    auto source = buffer(first, 16, BufferMemory::upload);
    auto local = take(first.begin());
    auto foreign = take(second.begin());
    require(!foreign.retain(source), "foreign resource accepted");
    require(!second.submit(std::move(local)), "foreign command batch accepted");
    auto ticket = take(first.submit(std::move(local)));
    require(!second.wait(ticket, 0), "foreign completion accepted");
    require(!first.wait(Submission{}, 0), "default completion accepted");
    require(take(first.wait(ticket)), "local completion failed");
}
void late_lifetime(memory::ResourceHandle resource, const DeviceOptions& options)
{
    std::array<std::byte, 64> input{}, actual{};
    input.fill(std::byte{0x5a});
    Buffer survivor;
    Submission expired;
    {
        auto owner = queue(resource, options);
        auto upload = buffer(owner, input.size(), BufferMemory::upload);
        survivor = buffer(owner, input.size(), BufferMemory::readback);
        check(upload.write(0, input));
        auto batch = take(owner.begin());
        check(batch.copy(upload, survivor, input.size()));
        expired = take(owner.submit(std::move(batch)));
        // Destruction drains pending work; survivor retains the device/allocator.
    }
    check(survivor.read(0, actual));
    require(actual == input, "queue destruction failed to drain work");
    survivor = Buffer{};
    CommandBatch retained;
    {
        auto owner = queue(resource, options);
        require(!owner.wait(expired, 0), "expired ticket reused by new queue");
        retained = take(owner.begin());
    }
    retained = CommandBatch{}; // Last batch releases its queue, without a lifetime cycle.
}
void allocation_failure(memory::MemorySystem& system, const DeviceOptions& options)
{
    auto resource = take(system.create_heap({"graphics-failure", memory::DomainCategory::render}));
    {
        auto owner = queue(resource, options);
        auto upload = buffer(owner, 16, BufferMemory::upload);
        auto batch = take(owner.begin());
        resource.begin_close();
        bool threw = false;
        try { check(batch.retain(upload)); } catch (const std::bad_alloc&) { threw = true; }
        require(threw, "closed CPU resource did not reject retain allocation");
        batch = CommandBatch{};
        require(owner.stats().submitted == 0 && owner.stats().free_slots == 2, "allocation exception published work");
        check(owner.close());
    }
    require(resource.snapshot().live_allocations == 0, "allocation failure leaked CPU ownership");
}
void lost_device(memory::ResourceHandle resource, const DeviceOptions& options)
{
    auto owner = queue(resource, options, true);
    auto batch = take(owner.begin());
    submit_error = VK_ERROR_DEVICE_LOST; // No real work is pending on this test device.
    require(!owner.submit(std::move(batch)), "device loss was ignored");
    require(owner.stats().device_lost && owner.stats().submitted == 0, "device loss published successful work");
    require(!owner.begin() && !owner.create_buffer({64}), "lost device accepted new work");
    require(!owner.close() && owner.stats().closed, "lost device did not close with an error");
}
void submit_without_cpu_allocation(memory::MemorySystem& system, const DeviceOptions& options)
{
    auto resource = take(system.create_heap({"graphics-commit", memory::DomainCategory::render}));
    {
        auto owner = queue(resource, options);
        std::array<std::byte, 16> data{}, actual{};
        data.fill(std::byte{0x37});
        auto upload = buffer(owner, data.size(), BufferMemory::upload);
        auto readback = buffer(owner, data.size(), BufferMemory::readback);
        check(upload.write(0, data));
        auto batch = take(owner.begin());
        check(batch.copy(upload, readback, data.size()));
        // All engine ownership/list allocation must be done before native submit.
        resource.begin_close();
        auto ticket = take(owner.submit(std::move(batch)));
        require(take(owner.wait(ticket)), "closed CPU heap prevented safe submission commit");
        check(readback.read(0, actual));
        require(actual == data, "allocation-free commit roundtrip failed");
        check(owner.close());
    }
    require(resource.snapshot().live_allocations == 0, "commit retained closed CPU heap allocations");
}
}
int main(int argc, char** argv)
{
    Diagnostics diagnostics;
    auto system = take(memory::MemorySystem::create());
    auto resource = take(system.create_heap({"graphics-resources", memory::DomainCategory::render}));
    DeviceOptions options;
    options.validation = argc == 2 && std::strcmp(argv[1], "--validation") == 0 ? ValidationMode::required : ValidationMode::disabled;
    options.diagnostic_sink = diagnostic;
    options.diagnostic_user_data = &diagnostics;
    auto initial = Device::create(resource, options);
    if (!initial) {
        std::fprintf(stderr, "%s\n", initial.error().message.c_str());
        return initial.error().code == ErrorCode::not_found || initial.error().code == ErrorCode::not_supported ? 77 : 1;
    }
    try {
        {
            require(!SubmissionQueue::create(resource, std::move(*initial), 0), "zero slots accepted");
            require(!SubmissionQueue::create(resource, std::move(*initial), 65), "unbounded slots accepted");
            native_submit = reinterpret_cast<PFN_vkQueueSubmit2>(initial->device_proc("vkQueueSubmit2"));
            native_wait = reinterpret_cast<PFN_vkWaitSemaphores>(initial->device_proc("vkWaitSemaphores"));
            auto owner = take(graphics::detail::SubmissionAccess::create(resource, std::move(*initial), 2,
                {submit_override, nullptr, wait_override}));
            const auto& info = owner.device().adapter();
            std::printf("GPU=%s driver=%s API=%u.%u.%u\n", info.properties.deviceName.data(), info.driver.driverInfo.data(),
                VK_API_VERSION_MAJOR(info.properties.apiVersion), VK_API_VERSION_MINOR(info.properties.apiVersion), VK_API_VERSION_PATCH(info.properties.apiVersion));
            require(!owner.create_buffer({}) && !owner.create_image({}), "invalid descriptions accepted");
            for (unsigned round = 0; round < 16; ++round) roundtrip(owner, round);
            require(allocations(owner) == 0, "roundtrip leaked VMA allocations");
            auto peer = queue(resource, options);
            cross_queue(owner, peer);
            check(owner.close());
            check(owner.close());
            require(!owner.begin() && !owner.create_buffer({64}), "closed queue accepted new work");
            std::printf("roundtrips=16 formats=5 completed=%llu pending=%u allocations=%u\n",
                static_cast<unsigned long long>(owner.stats().completed), owner.stats().pending_slots, allocations(owner));
        }
        late_lifetime(resource, options);
        allocation_failure(system, options);
        submit_without_cpu_allocation(system, options);
        lost_device(resource, options);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "graphics resources probe failed: %s\n", error.what());
        return 1;
    }
    initial = std::unexpected(Error{ErrorCode::invalid_state, "released"});
    std::printf("errors=%u warnings=%u liveAllocations=%zu\n", diagnostics.errors.load(), diagnostics.warnings.load(), resource.snapshot().live_allocations);
    return diagnostics.errors == 0 && diagnostics.warnings == 0 && resource.snapshot().live_allocations == 0 && system.try_close().closed() ? 0 : 1;
}
