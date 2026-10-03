#include "../RenderAssetSupport.hpp"
#include <dk/render/GpuAssets.hpp>
#include <dk/graphics/Transfer.hpp>
#include "SubmissionInternal.hpp"
#include "ObjectInternal.hpp"
#include <atomic>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <vector>

using namespace dk;
using namespace dk::graphics;
using namespace dk::render;
namespace {
void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
template<class T, class E> T take(std::expected<T,E>&& result) {
    if (!result) {
        if constexpr (std::is_same_v<E,Error>) {
            auto message = result.error().message;
            for (const auto& context : result.error().context) message += " / " + context;
            throw std::runtime_error(message);
        } else throw std::runtime_error("Memory operation failed");
    }
    return std::move(*result);
}
void check(Result<void> result) { if (!result) throw std::runtime_error(result.error().message); }
struct Diagnostics { std::atomic<unsigned> errors = 0, warnings = 0; };
void diagnostic(void* pointer, const Diagnostic& message) noexcept {
    auto& counts = *static_cast<Diagnostics*>(pointer);
    if (message.severity == VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) ++counts.errors;
    if (message.severity == VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) ++counts.warnings;
    if (message.severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
        std::fprintf(stderr,"%.*s: %.*s\n",static_cast<int>(message.name.size()),message.name.data(),
            static_cast<int>(message.message.size()),message.message.data());
}
PFN_vkQueueSubmit2 native_submit;
PFN_vkWaitSemaphores native_wait;
VkResult submit_error = VK_SUCCESS;
bool timeout_once = false;
memory::ResourceHandle close_at_submit;
VKAPI_ATTR VkResult VKAPI_CALL submit_override(VkQueue queue, std::uint32_t count, const VkSubmitInfo2* info, VkFence fence) {
    if (submit_error != VK_SUCCESS) return std::exchange(submit_error,VK_SUCCESS);
    const auto result = native_submit(queue,count,info,fence);
    if (result == VK_SUCCESS && close_at_submit) close_at_submit.begin_close();
    return result;
}
VKAPI_ATTR VkResult VKAPI_CALL wait_override(VkDevice device, const VkSemaphoreWaitInfo* info, std::uint64_t timeout) {
    if (std::exchange(timeout_once,false)) return VK_TIMEOUT;
    return native_wait(device,info,timeout);
}
std::uint32_t allocations(const SubmissionQueue& queue) {
    VmaTotalStatistics stats{}; vmaCalculateStatistics(queue.device().allocator(),&stats);
    return stats.total.statistics.allocationCount;
}
void bytes_equal(const ReadbackRequest& request, std::span<const std::byte> expected) {
    std::vector<std::byte> actual(expected.size());
    require(take(request.try_read(actual)),"readback not ready");
    require(std::ranges::equal(actual,expected),"GPU bytes differ from CPU input");
}
// Reads are queued immediately after upload, before any host wait.
void roundtrip(SubmissionQueue& queue, const CpuAsset& cpu, const GpuAsset& gpu) {
    require(gpu.mesh().primitives.size() == cpu.mesh.primitives.size() && gpu.textures().size() == cpu.textures.size(),"lost asset members");
    auto batch = take(queue.begin());
    std::vector<ReadbackRequest> reads;
    for (const auto& p : gpu.mesh().primitives) {
        require(take(p.vertices.state()).access == vk::AccessFlagBits2::eVertexAttributeRead,"vertex final access missing");
        require(take(p.indices.state()).access == vk::AccessFlagBits2::eIndexRead,"index final access missing");
        reads.push_back(take(batch.readback(p.vertices))); reads.push_back(take(batch.readback(p.indices)));
    }
    for (const auto& t : gpu.textures()) {
        require(take(t.image.state()).layout == vk::ImageLayout::eShaderReadOnlyOptimal,"texture final layout missing");
        require(t.image.description().format == vk::Format::eR8G8B8A8Srgb,"texture color space lost");
        require(t.sampler_description.min_filter == vk::Filter::eNearest && t.sampler_description.max_lod == 0 &&
            t.sampler_description.address_u == vk::SamplerAddressMode::eClampToEdge &&
            t.sampler_description.address_v == vk::SamplerAddressMode::eMirroredRepeat,"sampler mapping wrong");
        reads.push_back(take(batch.readback(t.image,{0,0,0,0,t.image.description().width,t.image.description().height})));
    }
    const auto ticket = take(queue.submit(std::move(batch)));
    require(take(queue.wait(ticket)),"readback wait failed");
    std::size_t index = 0;
    for (std::size_t n = 0; n < cpu.mesh.primitives.size(); ++n) {
        const auto& p = cpu.mesh.primitives[n]; const auto& g = gpu.mesh().primitives[n];
        std::vector<GpuVertex> expected(p.positions.size());
        for (std::size_t i = 0; i < expected.size(); ++i) {
            for (int c = 0; c < 3; ++c) {
                expected[i].position[c] = p.positions[i][c];
                expected[i].normal[c] = p.normals.empty() ? (c == 2 ? 1.0f : 0.0f) : p.normals[i][c];
            }
            for (int c = 0; c < 2; ++c) expected[i].uv[c] = p.texcoords.empty() ? 0 : p.texcoords[i][c];
        }
        bytes_equal(reads[index++],std::as_bytes(std::span{expected}));
        bytes_equal(reads[index++],std::as_bytes(std::span{p.indices}));
        require(g.has_normals == !p.normals.empty() && g.has_texcoords == !p.texcoords.empty(),"attribute presence lost");
        require(g.bounds_min.x() == -1 && g.bounds_max.y() == 1,"bounds not recomputed");
        require(g.material == p.material,"primitive material identity lost");
        if (p.material) require(gpu.materials()[n].base_color_texture == cpu.materials[n].base_color_texture,"material texture identity lost");
    }
    for (const auto& t : cpu.textures) bytes_equal(reads[index++],t.rgba8);
}
void lifecycle(memory::ResourceHandle heap, SubmissionQueue& queue) {
    auto cache = take(GpuAssets::create(heap)); auto cpu = render_asset();
    auto first = take(cache.upload(queue,cpu));
    require(queue.stats().pending_slots == 1,"upload unexpectedly waited");
    timeout_once = true; require(!take(first.wait(queue,0)),"timeout was reported as completion");
    require(cache.unload(cpu.mesh.id) && !cache.unload(cpu.mesh.id) && !cache.find(cpu.mesh.id),"unload failed");
    roundtrip(queue,cpu,first); require(take(first.wait(queue)),"upload not complete");
    cpu.mesh.primitives[0].positions[0].z() = 9; cpu.textures[0].rgba8[0] = std::byte{99};
    auto second = take(cache.upload(queue,cpu));
    require(second.generation() > first.generation() && first.mesh().primitives[0].vertices.handle() != second.mesh().primitives[0].vertices.handle(),"reload reused old version");
    roundtrip(queue,cpu,second);
    require(take(cache.find(cpu.mesh.id)).generation() == second.generation(),"cache did not publish replacement");
    // Old bytes remain accessible after replacement.
    auto batch = take(queue.begin()); auto old = take(batch.readback(first.textures()[0].image,{0,0,0,0,2,2}));
    require(take(queue.wait(take(queue.submit(std::move(batch))))),"old version wait failed");
    std::vector<std::byte> old_bytes(16); require(take(old.try_read(old_bytes)) && old_bytes[0] == std::byte{0},"reload mutated old bytes");
    first = {}; second = {}; old = {};
    auto pending = take(cache.upload(queue,cpu)); const auto ticket = pending.submission();
    cpu.mesh.primitives.clear(); cpu.textures.clear(); cpu.materials.clear();
    pending = {}; cache = {};
    require(queue.stats().pending_slots == 1 && allocations(queue) > 0,"pending resources freed early");
    require(take(queue.wait(ticket)),"pending release wait failed");
    require(allocations(queue) == 0,"unload leaked GPU allocations");
    // The procedural, untextured path has no images, materials or optional attributes.
    cpu = render_asset(); cpu.mesh.primitives.resize(1); cpu.mesh.primitives[0].material.reset();
    cpu.materials.clear(); cpu.textures.clear();
    cache = take(GpuAssets::create(heap)); auto simple = take(cache.upload(queue,cpu));
    require(simple.textures().empty() && simple.materials().empty(),"untextured upload invented assets");
    roundtrip(queue,cpu,simple); simple = {}; cache.clear();
    require(allocations(queue) == 0,"untextured upload leaked");
}
void failures(memory::ResourceHandle heap, SubmissionQueue& queue) {
    auto cpu = render_asset(); auto cache = take(GpuAssets::create(heap));
    auto old = take(cache.upload(queue,cpu)); require(take(old.wait(queue)),"initial wait failed");
    const auto base = allocations(queue); const auto submitted = queue.stats().submitted;
    auto unchanged = [&] {
        require(cache.size() == 1 && take(cache.find(cpu.mesh.id)).generation() == old.generation(),"failed upload replaced cache");
        require(allocations(queue) == base && queue.stats().recording_slots == 0 && queue.stats().submitted == submitted,"failed upload leaked/submitted");
    };
    cpu.mesh.primitives[0].indices[0] = 999; require(!cache.upload(queue,cpu),"bad indices accepted");
    cpu.mesh.primitives[0].indices[0] = 0; unchanged();
    graphics::detail::ObjectAccess::fail_creation(queue.resources(),VK_ERROR_OUT_OF_HOST_MEMORY);
    require(!cache.upload(queue,cpu),"object failure ignored"); unchanged();
    submit_error = VK_ERROR_OUT_OF_HOST_MEMORY;
    require(!cache.upload(queue,cpu),"submit failure ignored"); unchanged();
    auto fresh = take(cache.upload(queue,cpu)); require(fresh.generation() == old.generation() + 1,"failed attempts consumed generation");
    require(take(fresh.wait(queue)),"retry did not complete");
}
void budget_and_commit(memory::MemorySystem& system, SubmissionQueue& queue) {
    auto cpu = render_asset();
    auto heap = take(system.create_heap({"GPU-cache-budget",memory::DomainCategory::render,262144}));
    {
        auto cache = take(GpuAssets::create(heap)); auto old = take(cache.upload(queue,cpu)); require(take(old.wait(queue)),"initial budget wait failed");
        const auto base = allocations(queue);
        struct Block { void* pointer; std::size_t size; }; std::vector<Block> blocks;
        while (auto b = heap.try_allocate(1024)) blocks.push_back({*b,1024});
        while (auto b = heap.try_allocate(1)) blocks.push_back({*b,1});
        bool failed = false, partial = false, success = false;
        do {
            const auto before = heap.snapshot(); const auto submitted = queue.stats().submitted;
            {
                auto result = cache.upload(queue,cpu);
                if (!result) {
                    failed = true; partial |= heap.snapshot().allocation_count > before.allocation_count + 8;
                    require(take(cache.find(cpu.mesh.id)).generation() == old.generation(),"budget failure replaced old cache");
                    require(heap.snapshot().live_allocations == before.live_allocations &&
                        heap.snapshot().backing_requested_bytes == before.backing_requested_bytes,"budget candidate leaked Memory");
                    require(allocations(queue) == base && queue.stats().submitted == submitted,"budget failure leaked GPU or submitted");
                } else { success = true; require(take(result->wait(queue)),"budget retry failed"); }
            }
            if (success || blocks.empty()) break;
            auto b = blocks.back(); blocks.pop_back(); heap.deallocate(b.pointer,b.size);
        } while (true);
        for (auto b : blocks) heap.deallocate(b.pointer,b.size);
        require(failed && partial && success,"budget sweep lacked failure/partial/success coverage");
    }
    require(heap.snapshot().live_allocations == 0 && allocations(queue) == 0,"budget owners leaked");
    // Proves even first insertion commits without allocating after native submission.
    auto closing = take(system.create_heap({"close-at-submit",memory::DomainCategory::render}));
    {
        auto cache = take(GpuAssets::create(closing)); close_at_submit = closing;
        auto gpu = take(cache.upload(queue,cpu)); close_at_submit = {};
        require(closing.state() == memory::ResourceState::closing && cache.size() == 1,"commit after submit allocated or was lost");
        require(!cache.upload(queue,cpu),"closing heap accepted upload");
        require(take(gpu.wait(queue)),"closing snapshot could not wait"); cache.clear();
    }
    require(closing.snapshot().live_allocations == 0 && allocations(queue) == 0,"closing cache leaked");
}
}
int main(int argc, char** argv) {
    Diagnostics diagnostics;
    auto system = take(memory::MemorySystem::create());
    auto heap = take(system.create_heap({"render-probe",memory::DomainCategory::render}));
    DeviceOptions options;
    options.validation = argc == 2 && std::strcmp(argv[1],"--validation") == 0 ? ValidationMode::required : ValidationMode::disabled;
    options.diagnostic_sink = diagnostic; options.diagnostic_user_data = &diagnostics;
    auto device = Device::create(heap,options);
    if (!device) { std::fprintf(stderr,"%s\n",device.error().message.c_str());
        return device.error().code == ErrorCode::not_found || device.error().code == ErrorCode::not_supported ? 77 : 1; }
    try {
        {
            memory::ThreadContext context{system}; memory::ExecutionScope scope{context,heap};
            native_submit = reinterpret_cast<PFN_vkQueueSubmit2>(device->device_proc("vkQueueSubmit2"));
            native_wait = reinterpret_cast<PFN_vkWaitSemaphores>(device->device_proc("vkWaitSemaphores"));
            auto queue = take(graphics::detail::SubmissionAccess::create(heap,std::move(*device),3,{submit_override,nullptr,wait_override}));
            std::printf("GPU=%s driver=%s\n",queue.device().adapter().properties.deviceName.data(),queue.device().adapter().driver.driverInfo.data());
            lifecycle(heap,queue); failures(heap,queue);
            require(allocations(queue) == 0,"failure tests leaked");
            budget_and_commit(system,queue);
            GpuAsset survivor;
            {
                auto cache = take(GpuAssets::create(heap)); auto cpu = render_asset(); survivor = take(cache.upload(queue,cpu));
                auto other_device = take(Device::create(heap,options)); auto other_queue = take(SubmissionQueue::create(heap,std::move(other_device)));
                require(!survivor.wait(other_queue),"foreign queue accepted submission"); check(other_queue.close());
                check(queue.close()); require(!cache.upload(queue,cpu),"closed queue accepted upload");
                require(take(cache.find(cpu.mesh.id)).generation() == survivor.generation(),"closed queue upload changed cache");
            }
            require(queue.stats().pending_slots == 0,"queue close did not drain");
            survivor = {}; require(allocations(queue) == 0,"snapshot after queue close leaked");
            {
                auto final_device = take(Device::create(heap,options));
                auto final_queue = take(SubmissionQueue::create(heap,std::move(final_device)));
                auto cache = take(GpuAssets::create(heap)); auto cpu = render_asset();
                survivor = take(cache.upload(final_queue,cpu));
                require(final_queue.stats().pending_slots == 1,"final upload unexpectedly completed");
            } // Cache releases first; queue destructor drains while survivor keeps the native device alive.
            require(take(survivor.textures()[0].image.state()).initialized,"snapshot expired with queue owner");
            require(!survivor.wait(queue),"destroyed queue submission was accepted elsewhere");
            survivor = {};
            std::printf("mesh/texture roundtrips=3 pending=%u allocations=%u\n",queue.stats().pending_slots,allocations(queue));
        }
        device = std::unexpected(Error{ErrorCode::invalid_state,"released"});
        std::printf("errors=%u warnings=%u liveAllocations=%zu\n",diagnostics.errors.load(),diagnostics.warnings.load(),heap.snapshot().live_allocations);
        return diagnostics.errors == 0 && diagnostics.warnings == 0 && heap.snapshot().live_allocations == 0 && system.try_close().closed() ? 0 : 1;
    } catch (const std::exception& error) { std::fprintf(stderr,"render resources probe failed: %s\n",error.what()); return 1; }
}
