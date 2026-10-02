#include <dk/graphics/Resources.hpp>
#include <dk/graphics/CommandEncoder.hpp>
#include <dk/graphics/Transfer.hpp>
#include <dk/memory/MemorySystem.hpp>
#include "SubmissionInternal.hpp"
#include "ObjectInternal.hpp"
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
    require(take(image.state()).layout == vk::ImageLayout::eUndefined, "abandon published an image layout");
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
        require(take(image.state()).layout == vk::ImageLayout::eUndefined, "failed submit published layout");
        check(upload.write(0, input));
    }
    auto first = take(queue.begin());
    check(first.copy(upload, gpu, input.size()));
    check(first.copy_to_image(upload, image));
    check(first.transition(image, vk::ImageLayout::eShaderReadOnlyOptimal));
    auto first_ticket = take(queue.submit(std::move(first)));
    require(take(image.state()).layout == vk::ImageLayout::eShaderReadOnlyOptimal, "successful submit did not publish layout");
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
void subresource_commands(SubmissionQueue& owner)
{
    auto image = take(owner.create_image({8,8,vk::Format::eR8G8B8A8Unorm,
        vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eSampled,4,2}));
    auto upload = buffer(owner,64,BufferMemory::upload);
    auto readback = buffer(owner,64,BufferMemory::readback);
    const std::array<std::uint32_t,16> pixels{1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16};
    check(upload.write(0,std::as_bytes(std::span{pixels})));
    const vk::ImageSubresourceRange range{vk::ImageAspectFlagBits::eColor,1,1,1,1};
    const auto write = image_use(image,vk::PipelineStageFlagBits2::eCopy,vk::AccessFlagBits2::eTransferWrite,vk::ImageLayout::eTransferDstOptimal,range);
    const auto read = image_use(image,vk::PipelineStageFlagBits2::eCopy,vk::AccessFlagBits2::eTransferRead,vk::ImageLayout::eTransferSrcOptimal,range);
    {
        auto abandoned = take(owner.begin());
        const std::array writes{write};
        check(abandoned.prepare(writes));
    }
    require(take(image.state(1,1)).layout == vk::ImageLayout::eUndefined,"abandon published subresource state");
    {
        auto failed = take(owner.begin());
        const std::array writes{write};
        check(failed.prepare(writes));
        submit_error = VK_ERROR_OUT_OF_HOST_MEMORY;
        require(!owner.submit(std::move(failed)),"failed subresource submit accepted");
    }
    require(take(image.state(1,1)).layout == vk::ImageLayout::eUndefined,"failed submit published subresource state");
    auto batch = take(owner.begin());
    const std::array initial{buffer_use(upload,vk::PipelineStageFlagBits2::eCopy,vk::AccessFlagBits2::eTransferRead),write};
    auto wrong = ResourceBarrier{read,{vk::PipelineStageFlagBits2::eCopy,vk::AccessFlagBits2::eTransferWrite,vk::ImageLayout::eGeneral}};
    const std::array wrong_barriers{wrong};
    require(!batch.barrier(wrong_barriers),"Graph barrier accepted mismatched before state");
    check(upload.write(0,std::as_bytes(std::span{pixels}))); // Rejected plan leaves no reservation.
    check(batch.prepare(initial));
    auto competing = take(owner.begin());
    require(!competing.retain(upload),"two buffer recording reservations accepted");
    competing = {};
    // A partial write cannot establish valid content for a previously undefined mip.
    check(batch.copy_to_image(upload,image,{1,1,0,0,2,2}));
    const std::array reading{read,buffer_use(readback,vk::PipelineStageFlagBits2::eCopy,vk::AccessFlagBits2::eTransferWrite)};
    check(batch.prepare(reading));
    require(!batch.copy_to_buffer(image,readback,{1,1,0,0,4,4}),"partial initialization accepted as full content");
    check(batch.prepare(initial));
    check(batch.copy_to_image(upload,image,{1,1,0,0,4,4}));
    const std::array graph_barriers{ResourceBarrier{read,{vk::PipelineStageFlagBits2::eCopy,vk::AccessFlagBits2::eTransferWrite,vk::ImageLayout::eTransferDstOptimal,true}}};
    check(batch.barrier(graph_barriers));
    check(batch.copy_to_buffer(image,readback,{1,1,0,0,4,4}));
    const std::array host{buffer_use(readback,vk::PipelineStageFlagBits2::eHost,vk::AccessFlagBits2::eHostRead)};
    check(batch.prepare(host));
    auto stale = take(batch.compute());
    auto moved = std::move(batch);
    require(!stale.dispatch({1,1,1}),"batch move left encoder valid");
    auto ticket = take(owner.submit(std::move(moved)));
    require(take(image.state(0,0)).layout == vk::ImageLayout::eUndefined && take(image.state(1,0)).layout == vk::ImageLayout::eUndefined &&
        take(image.state(1,1)).layout == vk::ImageLayout::eTransferSrcOptimal,"subresource state contaminated other mip/layer");
    require(take(owner.wait(ticket)),"subresource transfer timed out");
    std::array<std::uint32_t,16> actual{};
    check(readback.read(0,std::as_writable_bytes(std::span{actual})));
    require(actual == pixels,"subresource copy mismatch");
    {
        auto unsafe = take(owner.begin());
        auto old_encoder = take(unsafe.compute());
        check(unsafe.unsafe_record({}, {}, [](const vk::raii::CommandBuffer&,void*) {}));
        require(!old_encoder.dispatch({1,1,1}),"native escape did not invalidate encoder");
        bool threw = false;
        try { (void)unsafe.unsafe_record({}, {}, [](const vk::raii::CommandBuffer&,void*) { throw std::runtime_error("injected"); }); }
        catch (const std::runtime_error&) { threw = true; }
        require(threw && !owner.submit(std::move(unsafe)),"throwing native recording remained submittable");
    }
}
void transfer_requests(memory::ResourceHandle resource, const DeviceOptions& options)
{
    ReadbackRequest survivor;
    const std::array<std::uint32_t,16> data{1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16};
    {
        auto owner = queue(resource,options,true);
        auto gpu = buffer(owner,64,BufferMemory::device);
        auto image = take(owner.create_image({8,8,vk::Format::eR8G8B8A8Unorm,
            vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eTransferDst,2,2}));
        std::array<std::uint32_t,64> base{};
        for (std::uint32_t i = 0; i < base.size(); ++i) base[i] = 100+i;
        std::array<std::uint32_t,22> pitched{};
        for (std::uint32_t y = 0; y < 4; ++y) for (std::uint32_t x = 0; x < 4; ++x) pitched[y*6+x] = data[y*4+x];
        auto batch = take(owner.begin());
        check(batch.upload(gpu,std::as_bytes(std::span{data}).first(32)));
        check(batch.upload(gpu,std::as_bytes(std::span{data}).subspan(32),32));
        check(batch.upload(image,std::as_bytes(std::span{base}),{0,0,0,0,8,8}));
        check(batch.upload(image,std::as_bytes(std::span{pitched}),{1,1,0,0,4,4,0,6}));
        auto bytes = take(batch.readback(gpu));
        auto subregion = take(batch.readback(image,{0,0,1,2,2,2}));
        auto mip = take(batch.readback(image,{1,1,0,0,4,4}));
        auto dropped = take(batch.readback(gpu));
        dropped = {}; // Pending ownership must not depend on the user's request.
        require(owner.stats().submitted == 0,"upload/readback submitted implicitly");
        std::array<std::uint32_t,16> actual{};
        actual.fill(0xccccccccu);
        require(bytes.status() == ReadbackStatus::unsubmitted && !take(bytes.try_read(std::as_writable_bytes(std::span{actual}))) && actual[0] == 0xccccccccu,
            "unsubmitted request returned bytes");
        auto ticket = take(owner.submit(std::move(batch)));
        require(owner.stats().submitted == 1 && bytes.status() == ReadbackStatus::pending,"batch did not bind requests at commit");
        const auto pending_allocations = allocations(owner);
        timeout_once = true;
        require(!take(owner.wait(ticket,0)) && allocations(owner) == pending_allocations && !take(bytes.try_read(std::as_writable_bytes(std::span{actual}))),
            "timeout completed/released a readback");
        require(take(owner.wait(ticket)),"transfer batch wait failed");
        require(bytes.status() == ReadbackStatus::ready && take(bytes.try_read(std::as_writable_bytes(std::span{actual}))) && actual == data,"batched buffer uploads mismatch");
        require(take(mip.try_read(std::as_writable_bytes(std::span{actual}))) && actual == data,"pitched mip/layer upload mismatch");
        std::array<std::uint32_t,4> cropped{};
        require(subregion.description().row_pitch == 8 && take(subregion.try_read(std::as_writable_bytes(std::span{cropped}))),"subregion readback metadata failed");
        require(cropped == std::array<std::uint32_t,4>{117,118,125,126},"subregion readback pixels mismatch");
        ReadbackRequest cancelled;
        {
            auto abandoned = take(owner.begin());
            cancelled = take(abandoned.readback(gpu));
        }
        require(cancelled.status() == ReadbackStatus::cancelled && !cancelled.try_read(std::as_writable_bytes(std::span{actual})),"abandoned readback remained live");
        auto failed = take(owner.begin());
        auto rejected = take(failed.readback(gpu));
        submit_error = VK_ERROR_OUT_OF_HOST_MEMORY;
        require(!owner.submit(std::move(failed)) && rejected.status() == ReadbackStatus::cancelled,"failed submit published readback completion");
        auto last = take(owner.begin());
        survivor = take(last.readback(gpu));
        (void)take(owner.submit(std::move(last)));
    }
    std::array<std::uint32_t,16> actual{};
    require(survivor.status() == ReadbackStatus::ready && take(survivor.try_read(std::as_writable_bytes(std::span{actual}))) && actual == data,
        "queue destruction did not complete independent readback");
    survivor = {};
    {
        auto owner = queue(resource,options);
        auto gpu = buffer(owner,64,BufferMemory::device);
        auto image = take(owner.create_image({4,4}));
        auto batch = take(owner.begin());
        const auto fill_use = buffer_use(gpu,vk::PipelineStageFlagBits2::eClear,vk::AccessFlagBits2::eTransferWrite);
        const auto clear_use = image_use(image,vk::PipelineStageFlagBits2::eClear,vk::AccessFlagBits2::eTransferWrite,vk::ImageLayout::eTransferDstOptimal);
        const std::array uses{fill_use,clear_use};
        check(batch.prepare(uses));
        check(batch.fill(gpu,0x12345678u));
        require(!batch.fill(gpu),"second fill accepted without write barrier");
        check(batch.clear(image,vk::ClearColorValue{std::array<float,4>{0,1,0,1}},clear_use.range));
        auto fill_request = take(batch.readback(gpu));
        auto clear_request = take(batch.readback(image,{0,0,0,0,4,4}));
        auto ticket = take(owner.submit(std::move(batch)));
        require(take(owner.wait(ticket)),"fill/clear timed out");
        require(take(fill_request.try_read(std::as_writable_bytes(std::span{actual}))) && actual[0] == 0x12345678u && actual[15] == 0x12345678u,"buffer fill mismatch");
        require(take(clear_request.try_read(std::as_writable_bytes(std::span{actual}))) && actual[0] == 0xff00ff00u && actual[15] == 0xff00ff00u,"image clear mismatch");
        auto upload = buffer(owner,64,BufferMemory::upload);
        check(upload.write(0,std::as_bytes(std::span{data})));
        auto mixed = take(owner.begin());
        check(mixed.copy_to_image(upload,image)); // Compatibility path must publish conservative source reads.
        const std::array overwrite{buffer_use(upload,vk::PipelineStageFlagBits2::eClear,vk::AccessFlagBits2::eTransferWrite)};
        check(mixed.prepare(overwrite));
        check(mixed.fill(upload));
        auto original = take(mixed.readback(image,{0,0,0,0,4,4}));
        auto mixed_ticket = take(owner.submit(std::move(mixed)));
        require(take(owner.wait(mixed_ticket)) && take(original.try_read(std::as_writable_bytes(std::span{actual}))) && actual == data,
            "legacy upload and typed overwrite lost WAR dependency");
    }
    {
        auto owner = queue(resource,options,true);
        auto gpu = buffer(owner,64,BufferMemory::device);
        auto batch = take(owner.begin());
        check(batch.upload(gpu,std::as_bytes(std::span{data})));
        auto lost = take(batch.readback(gpu));
        auto ticket = take(owner.submit(std::move(batch)));
        owner.device().queue().waitIdle(); // Safely inject loss after real work has stopped.
        wait_error = VK_ERROR_DEVICE_LOST;
        require(!owner.wait(ticket) && lost.status() == ReadbackStatus::device_lost && !lost.try_read(std::as_writable_bytes(std::span{actual})),"lost request returned successful bytes");
        require(!owner.close(),"lost queue close reported success");
    }
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
void object_factory(memory::ResourceHandle resource, const DeviceOptions& options)
{
    Sampler sampler;
    ShaderModule shader;
    ImageView view;
    ResourceFactory expired;
    {
        auto owner = queue(resource, options);
        auto factory = owner.resources();
        expired = factory;
        auto image = take(factory.create_image({8, 8}));
        view = take(factory.create_view(image));
        image = Image{};
        require(allocations(owner) == 1, "view did not retain image allocation");
        auto peer = queue(resource, options);
        auto foreign = take(peer.create_image({8, 8}));
        require(!factory.create_view(foreign), "foreign image accepted by factory");
        SamplerDesc invalid;
        invalid.max_lod = -1;
        require(!factory.create_sampler(invalid), "negative sampler LOD accepted");
        sampler = take(factory.create_sampler());
        sampler = take(factory.create_sampler()); // Move assignment must destroy previous sampler.
        CompiledShader artifact{resource};
        artifact.entry = "main";
        artifact.thread_group_size = {1, 1, 1};
        artifact.spirv = {0x07230203, 0x00010500, 0, 6, 0,
            0x00020011, 1, 0x0003000e, 0, 1,
            0x0005000f, 5, 4, 0x6e69616d, 0,
            0x00060010, 4, 17, 1, 1, 1,
            0x00020013, 1, 0x00030021, 2, 1,
            0x00050036, 1, 4, 0, 2, 0x000200f8, 5, 0x000100fd, 0x00010038};
        shader = take(factory.create_shader(artifact));
        const auto live = resource.snapshot().live_allocations;
        graphics::detail::ObjectAccess::fail_creation(factory, VK_ERROR_OUT_OF_HOST_MEMORY);
        require(!factory.create_view(foreign), "foreign view accepted before fault seam");
        require(!factory.create_sampler(), "post-create failure was ignored");
        require(resource.snapshot().live_allocations == live, "failed creation leaked object state");
        graphics::detail::ObjectAccess::fail_creation(factory, VK_ERROR_OUT_OF_HOST_MEMORY);
        require(!factory.create_shader(artifact), "shader post-create failure was ignored");
        require(resource.snapshot().live_allocations == live, "failed shader creation leaked");
        check(owner.close());
        require(!factory.create_sampler(), "closed factory accepted new object");
    }
    require(!expired.create_sampler(), "factory outlived queue and accepted work");
    expired = {};
    require(shader.entry() == "main" && bool(shader.handle()) && bool(view.handle()), "owned metadata or handles lost");
    view = {}; // Now only non-VMA objects retain the first device.
    {
        auto peer = queue(resource, options);
        auto other = take(peer.resources().create_sampler());
    }
    shader = {};
    sampler = {}; // Correct dispatcher and device must still be alive here.
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
        auto gpu = buffer(owner, data.size(), BufferMemory::device);
        check(upload.write(0, data));
        auto batch = take(owner.begin());
        check(batch.upload(gpu,data));
        auto readback = take(batch.readback(gpu));
        // All engine ownership/list allocation must be done before native submit.
        resource.begin_close();
        auto ticket = take(owner.submit(std::move(batch)));
        require(take(owner.wait(ticket)), "closed CPU heap prevented safe submission commit");
        require(take(readback.try_read(actual)), "allocation-free request did not become ready");
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
            subresource_commands(owner);
            auto peer = queue(resource, options);
            cross_queue(owner, peer);
            check(owner.close());
            check(owner.close());
            require(!owner.begin() && !owner.create_buffer({64}), "closed queue accepted new work");
            std::printf("roundtrips=16 formats=5 completed=%llu pending=%u allocations=%u\n",
                static_cast<unsigned long long>(owner.stats().completed), owner.stats().pending_slots, allocations(owner));
        }
        late_lifetime(resource, options);
        object_factory(resource, options);
        transfer_requests(resource, options);
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
