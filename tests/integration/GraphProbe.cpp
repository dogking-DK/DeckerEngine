#include <dk/graphics/GraphExecution.hpp>
#include <dk/memory/MemorySystem.hpp>
#include "SubmissionInternal.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <vector>

using namespace dk;
using namespace dk::graphics;
using namespace dk::graphics::graph;
namespace {
void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
template<class T, class E> T take(std::expected<T, E>&& result) {
    if (!result) {
        if constexpr (std::is_same_v<E, Error>) throw std::runtime_error(result.error().message);
        else throw std::runtime_error("Memory allocation failed");
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
        std::fprintf(stderr, "%.*s: %.*s\n", static_cast<int>(message.name.size()), message.name.data(),
            static_cast<int>(message.message.size()), message.message.data());
}
PFN_vkQueueSubmit2 native_submit;
PFN_vkWaitSemaphores native_wait;
VkResult submit_error = VK_SUCCESS;
bool timeout_once = false;
memory::ResourceHandle close_at_submit;
VKAPI_ATTR VkResult VKAPI_CALL submit_override(VkQueue queue, std::uint32_t count, const VkSubmitInfo2* info, VkFence fence) {
    if (submit_error != VK_SUCCESS) return std::exchange(submit_error, VK_SUCCESS);
    const auto result = native_submit(queue, count, info, fence);
    if (result == VK_SUCCESS && close_at_submit) close_at_submit.begin_close();
    return result;
}
VKAPI_ATTR VkResult VKAPI_CALL wait_override(VkDevice device, const VkSemaphoreWaitInfo* info, std::uint64_t timeout) {
    if (std::exchange(timeout_once, false)) return VK_TIMEOUT;
    return native_wait(device, info, timeout);
}
std::uint32_t allocations(const SubmissionQueue& queue) {
    VmaTotalStatistics stats{}; vmaCalculateStatistics(queue.device().allocator(), &stats);
    return stats.total.statistics.allocationCount;
}
Use read(BufferId id, vk::DeviceSize offset = 0, vk::DeviceSize size = VK_WHOLE_SIZE) {
    return {id, {{vk::PipelineStageFlagBits2::eCopy, vk::AccessFlagBits2::eTransferRead}, offset, size}};
}
Use write(BufferId id, vk::PipelineStageFlags2 stage = vk::PipelineStageFlagBits2::eCopy,
    vk::DeviceSize offset = 0, vk::DeviceSize size = VK_WHOLE_SIZE) {
    return {id, {{stage, vk::AccessFlagBits2::eTransferWrite}, offset, size, {}, true}};
}
Use image_use_for(ImageId id, bool writing, vk::ImageSubresourceRange range = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1}) {
    return {id, {{vk::PipelineStageFlagBits2::eCopy, writing ? vk::AccessFlagBits2::eTransferWrite : vk::AccessFlagBits2::eTransferRead,
        writing ? vk::ImageLayout::eTransferDstOptimal : vk::ImageLayout::eTransferSrcOptimal}, 0, VK_WHOLE_SIZE, range, writing}};
}
void add(Graph& graph, std::string_view name, std::initializer_list<Use> uses, bool effect = false) {
    static_cast<void>(take(graph.add_pass({name, {uses.begin(), uses.size()}, effect})));
}
Result<void> noop(PassContext&, void*) { return {}; }
Result<void> fail(PassContext&, void*) { return std::unexpected(Error{ErrorCode::conflict, "injected recorder failure", {"inner recorder"}}); }
Result<void> throws(PassContext&, void*) { throw std::runtime_error("injected recorder exception"); }
Result<void> fill_first(PassContext& pass, void*) { return pass.fill(0, 0x12345678u); }

void roundtrip(memory::ResourceHandle heap, SubmissionQueue& queue) {
    auto graph = take(Graph::create(heap));
    const BufferDesc upload_desc{64, vk::BufferUsageFlagBits::eTransferSrc, BufferMemory::upload};
    const BufferDesc output_desc{64, vk::BufferUsageFlagBits::eTransferDst, BufferMemory::readback};
    auto upload = take(queue.create_buffer(upload_desc));
    auto output = take(queue.create_buffer(output_desc));
    const auto source = take(graph.declare_buffer("upload", upload_desc, Lifetime::external, true)); // 0
    const auto temporary = take(graph.declare_buffer("temporary", {64})); // 1
    const auto result = take(graph.declare_buffer("readback", output_desc, Lifetime::external)); // 2
    const auto dead = take(graph.declare_buffer("culled", {64})); // 3
    const auto color = take(graph.declare_image("color", {4, 4})); // 4
    add(graph, "upload", {read(source), write(temporary)});
    add(graph, "to image", {read(temporary), image_use_for(color, true)});
    add(graph, "readback", {image_use_for(color, false), write(result)});
    add(graph, "unused", {write(dead, vk::PipelineStageFlagBits2::eClear)});
    check(graph.mark_output(result)); check(graph.mark_output(color));
    const auto plan = take(graph.compile());
    require(plan.order().size() == 3, "dead pass was retained");
    struct Counter { unsigned recorded = 0; } counter;
    const std::array callbacks{
        PassCallback{0, [](PassContext& pass, void* data) -> Result<void> {
            ++static_cast<Counter*>(data)->recorded;
            require(!pass.buffer(3) && !pass.image(0), "undeclared/type-mismatched resource exposed");
            return pass.copy_buffer(0, 1, 64);
        }, &counter},
        PassCallback{1, [](PassContext& pass, void*) { return pass.copy_to_image(1, 4, {0, 0, 0, 0, 4, 4}); }},
        PassCallback{2, [](PassContext& pass, void*) { return pass.copy_to_buffer(4, 2, {0, 0, 0, 0, 4, 4}); }},
        PassCallback{3, throws}};
    const std::array bindings{ExternalBinding{0, &upload}, ExternalBinding{2, &output}};
    const std::array final{
        FinalAccess{2, {{vk::PipelineStageFlagBits2::eHost, vk::AccessFlagBits2::eHostRead}}},
        FinalAccess{4, {{vk::PipelineStageFlagBits2::eFragmentShader, vk::AccessFlagBits2::eShaderSampledRead, vk::ImageLayout::eShaderReadOnlyOptimal}}}};
    for (unsigned round = 0; round < 4; ++round) {
        std::array<std::uint32_t, 16> input{}, actual{};
        for (std::size_t i = 0; i < input.size(); ++i) input[i] = static_cast<std::uint32_t>(i * 7919 + round * 97);
        check(upload.write(0, std::as_bytes(std::span{input})));
        auto execution = take(execute(plan, queue, {bindings, callbacks, final, true}));
        require(execution.synchronization().size() == 8,"missing graph barrier diagnostics");
        const auto& image_barrier = execution.synchronization()[3];
        require(image_barrier.pass == 1 && image_barrier.resource == 4 &&
            image_barrier.before.layout == vk::ImageLayout::eUndefined && image_barrier.target.layout == vk::ImageLayout::eTransferDstOptimal &&
            !image_barrier.target.initialized,"image barrier diagnostics invented initial content");
        require(!execution.synchronization().back().pass,"final barrier was labelled as a pass");
        require(counter.recorded == round + 1, "callback execution count mismatch");
        require(execution.states().size() == 3, "missing exported state");
        require(!execution.buffer(1) && !execution.buffer(3), "non-output was retained by execution");
        require(take(execution.image(4))->handle() != vk::Image{}, "image output missing");
        require(execution.states().back().state.layout == vk::ImageLayout::eShaderReadOnlyOptimal, "final image layout not exported");
        require(take(output.state()).stages == vk::PipelineStageFlagBits2::eHost, "final host state not published");
        require(!output.read(0, std::as_writable_bytes(std::span{actual})), "pending readback allowed");
        const auto live = allocations(queue);
        require(live == 4, "transient allocation or culling mismatch");
        auto image_owner = take(execution.image(4))->share();
        const auto ticket = execution.submission();
        execution = Execution{};
        require(allocations(queue) == live, "destroyed pending execution freed resources");
        timeout_once = true;
        require(!take(queue.wait(ticket, 0)), "timeout was not reported");
        require(allocations(queue) == live, "timeout reclaimed resources");
        require(take(queue.wait(ticket)), "execution did not complete");
        require(allocations(queue) == 3, "completion retained temporary resource");
        check(output.read(0, std::as_writable_bytes(std::span{actual})));
        require(input == actual, "GPU graph roundtrip differs");
        require(take(image_owner.state()).layout == vk::ImageLayout::eShaderReadOnlyOptimal, "shared output state lost");
    }
}

void import_and_failures(memory::ResourceHandle heap, SubmissionQueue& queue, const DeviceOptions& options) {
    const BufferDesc desc{64, vk::BufferUsageFlagBits::eTransferSrc | vk::BufferUsageFlagBits::eTransferDst, BufferMemory::readback};
    auto output = take(queue.create_buffer(desc));
    auto graph = take(Graph::create(heap));
    const auto id = take(graph.declare_buffer("external output", desc, Lifetime::external));
    add(graph, "fill", {write(id, vk::PipelineStageFlagBits2::eClear)});
    check(graph.mark_output(id));
    const auto plan = take(graph.compile());
    const std::array bindings{ExternalBinding{0, &output}};
    std::array callbacks{PassCallback{0, fill_first}};
    const auto before = take(output.state());
    const auto submitted = queue.stats().submitted;
    const auto live = allocations(queue);
    const auto rejects = [&](const ExecutionDesc& request) {
        const auto execution = execute(plan, queue, request);
        require(!execution, "invalid execution accepted");
        require(queue.stats().submitted == submitted && queue.stats().recording_slots == 0, "failure published or retained work");
        require(take(output.state()) == before && allocations(queue) == live, "failure changed state or leaked GPU allocations");
        return execution.error();
    };
    rejects({{}, callbacks, {}}); rejects({bindings, {}, {}});
    const std::array duplicate_callbacks{callbacks[0], callbacks[0]}; rejects({bindings, duplicate_callbacks, {}});
    const std::array duplicate_bindings{bindings[0], bindings[0]}; rejects({duplicate_bindings, callbacks, {}});
    const std::array stale{AccessState{vk::PipelineStageFlagBits2::eHost, vk::AccessFlagBits2::eHostWrite}};
    const std::array stale_bindings{ExternalBinding{0, &output, nullptr, stale}}; rejects({stale_bindings, callbacks, {}});
    auto wrong = take(queue.create_buffer({32}));
    const std::array wrong_binding{ExternalBinding{0, &wrong}};
    require(!execute(plan, queue, {wrong_binding, callbacks, {}}), "wrong description accepted"); wrong = Buffer{};
    const FinalAccess final{0, {{vk::PipelineStageFlagBits2::eHost, vk::AccessFlagBits2::eHostRead}}};
    const std::array overlapping{final, final}; rejects({bindings, callbacks, overlapping});
    callbacks[0].record = fail;
    const auto error = rejects({bindings, callbacks, {}});
    require(error.code == ErrorCode::conflict && error.message == "injected recorder failure" && error.context.size() == 2 &&
        error.context.front() == "inner recorder" && error.context.back().find("graph pass #0 'fill'") != std::string::npos,"graph lost nested error context");
    callbacks[0].record = throws; rejects({bindings, callbacks, {}});
    callbacks[0].record = noop; rejects({bindings, callbacks, {}}); // An unrecorded write cannot masquerade as initialization.
    callbacks[0].record = fill_first;
    submit_error = VK_ERROR_OUT_OF_HOST_MEMORY; rejects({bindings, callbacks, {}});
    {
        auto competing = take(queue.begin()); check(competing.retain(output));
        require(!execute(plan, queue, {bindings, callbacks, {}}), "recording reservation ignored");
    }
    {
        auto device = take(Device::create(heap, options));
        auto peer = take(SubmissionQueue::create(heap, std::move(device)));
        require(!execute(plan, peer, {bindings, callbacks, {}}), "foreign device accepted");
        check(peer.close());
    }
    auto execution = take(execute(plan, queue, {bindings, callbacks, {}}));
    auto moved = std::move(execution);
    require(!execution && execution.states().empty() && !execution.buffer(0), "moved execution remained accessible");
    require(take(queue.wait(moved.submission())), "fill did not complete");
    std::array<std::uint32_t, 16> actual{}; check(output.read(0, std::as_writable_bytes(std::span{actual})));
    require(std::ranges::all_of(actual, [](auto value) { return value == 0x12345678u; }), "fill output differs");
    const auto snapshot = moved.states()[0].state;
    auto imported_graph = take(Graph::create(heap));
    const auto imported_id = take(imported_graph.declare_buffer("retained output", desc, Lifetime::external, true));
    check(imported_graph.mark_output(imported_id));
    const auto imported_plan = take(imported_graph.compile());
    const std::array expected{take(output.state())};
    const std::array imported{ExternalBinding{0, &output, nullptr, expected}};
    const std::array final_access{final};
    auto second = take(execute(imported_plan, queue, {imported, {}, final_access}));
    require(moved.states()[0].state == snapshot, "later execution changed prior snapshot");
    require(take(queue.wait(second.submission())), "empty-pass export did not complete");
    require(!execute(imported_plan, queue, {imported, {}, {}}), "stale exact import accepted after transition");
    auto uninitialized = take(queue.create_buffer(desc));
    const std::array missing_content{ExternalBinding{0, &uninitialized}};
    require(!execute(imported_plan, queue, {missing_content, {}, {}}), "uninitialized import accepted");
    auto aliases = take(Graph::create(heap));
    const auto a = take(aliases.declare_buffer("a", desc, Lifetime::external, true));
    const auto b = take(aliases.declare_buffer("b", desc, Lifetime::external, true));
    check(aliases.mark_output(a)); check(aliases.mark_output(b));
    const auto alias_plan = take(aliases.compile());
    const std::array aliased{ExternalBinding{0, &output}, ExternalBinding{1, &output}};
    require(!execute(alias_plan, queue, {aliased, {}, {}}), "physical resource alias accepted");
}

void subresources_and_hazards(memory::ResourceHandle heap, SubmissionQueue& queue) {
    auto graph = take(Graph::create(heap));
    const BufferDesc desc{16, vk::BufferUsageFlagBits::eTransferSrc | vk::BufferUsageFlagBits::eTransferDst, BufferMemory::readback};
    const auto first = take(graph.declare_buffer("first", desc)); // 0
    const auto second = take(graph.declare_buffer("second", desc)); // 1
    const auto image = take(graph.declare_image("mips", {4, 4, vk::Format::eR8G8B8A8Unorm,
        vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eTransferDst, 2, 2})); // 2
    const vk::ImageSubresourceRange target{vk::ImageAspectFlagBits::eColor, 1, 1, 1, 1};
    auto clear = image_use_for(image, true, target); clear.access.state.stages = vk::PipelineStageFlagBits2::eClear;
    add(graph, "first clear", {clear});
    add(graph, "first read", {image_use_for(image, false, target), write(first)});
    add(graph, "second clear", {clear});
    add(graph, "second read", {image_use_for(image, false, target), write(second)});
    check(graph.mark_output(first)); check(graph.mark_output(second));
    const auto plan = take(graph.compile());
    const std::array callbacks{
        PassCallback{0, [](PassContext& pass, void*) { return pass.clear(2, vk::ClearColorValue{std::array<float,4>{1,0,0,1}}, {vk::ImageAspectFlagBits::eColor,1,1,1,1}); }},
        PassCallback{1, [](PassContext& pass, void*) { return pass.copy_to_buffer(2, 0, {1,1,0,0,2,2}); }},
        PassCallback{2, [](PassContext& pass, void*) { return pass.clear(2, vk::ClearColorValue{std::array<float,4>{0,1,0,1}}, {vk::ImageAspectFlagBits::eColor,1,1,1,1}); }},
        PassCallback{3, [](PassContext& pass, void*) { return pass.copy_to_buffer(2, 1, {1,1,0,0,2,2}); }}};
    auto execution = take(execute(plan, queue, {{}, callbacks, {}}));
    require(take(queue.wait(execution.submission())), "subresource graph did not complete");
    std::array<std::uint32_t,4> a{}, b{};
    check(take(execution.buffer(0))->read(0, std::as_writable_bytes(std::span{a})));
    check(take(execution.buffer(1))->read(0, std::as_writable_bytes(std::span{b})));
    require(std::ranges::all_of(a, [](auto value) { return value == 0xff0000ffu; }), "RAW/WAR first image value differs");
    require(std::ranges::all_of(b, [](auto value) { return value == 0xff00ff00u; }), "WAW second image value differs");
    require(allocations(queue) == 2, "temporary image lifetime incorrect");
}

void imported_subresources(memory::ResourceHandle heap, SubmissionQueue& queue) {
    auto image = take(queue.create_image({4,4,vk::Format::eR8G8B8A8Unorm,
        vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eSampled,2,2}));
    auto graph = take(Graph::create(heap));
    const auto id = take(graph.declare_image("external mips", image.description(), Lifetime::external));
    auto write_use = image_use_for(id,true,{vk::ImageAspectFlagBits::eColor,0,2,0,2});
    write_use.access.state.stages = vk::PipelineStageFlagBits2::eClear;
    add(graph,"initialize all",{write_use}); check(graph.mark_output(id));
    const auto plan = take(graph.compile());
    const std::array initial{take(image.state(0,0)),take(image.state(1,0)),take(image.state(0,1)),take(image.state(1,1))};
    const std::array bindings{ExternalBinding{0,nullptr,&image,initial}};
    const std::array callbacks{PassCallback{0,[](PassContext& pass,void*) {
        return pass.clear(0,vk::ClearColorValue{std::array<float,4>{0,0,1,1}},{vk::ImageAspectFlagBits::eColor,0,2,0,2});
    }}};
    const std::array final{
        FinalAccess{0,{{vk::PipelineStageFlagBits2::eFragmentShader,vk::AccessFlagBits2::eShaderSampledRead,vk::ImageLayout::eShaderReadOnlyOptimal},
            0,VK_WHOLE_SIZE,{vk::ImageAspectFlagBits::eColor,0,2,0,1}}},
        FinalAccess{0,{{vk::PipelineStageFlagBits2::eCopy,vk::AccessFlagBits2::eTransferRead,vk::ImageLayout::eTransferSrcOptimal},
            0,VK_WHOLE_SIZE,{vk::ImageAspectFlagBits::eColor,0,2,1,1}}}};
    auto execution = take(execute(plan,queue,{bindings,callbacks,final}));
    require(execution.states().size() == 4,"image state export count differs");
    for (std::size_t i = 0; i < 4; ++i) {
        const auto& value = execution.states()[i];
        require(value.mip == i % 2 && value.layer == i / 2 && value.state.initialized,"image state order/initialization differs");
        require(value.state == take(image.state(value.mip,value.layer)),"local export differs from submitted image ledger");
        require(value.state.layout == (i < 2 ? vk::ImageLayout::eShaderReadOnlyOptimal : vk::ImageLayout::eTransferSrcOptimal),"subresource final layout differs");
    }
    require(take(queue.wait(execution.submission())),"imported image execution did not complete");
    require(!execute(plan,queue,{bindings,callbacks,final}),"stale image subresource import accepted");
    image = Image{}; // The result owns an explicit alias of the external output.
    require(take(execution.image(0))->handle() != vk::Image{},"external image output ownership lost");
}

void encoder_boundary(memory::ResourceHandle heap, SubmissionQueue& queue) {
    auto graph = take(Graph::create(heap));
    add(graph,"encoder",{},true); add(graph,"check expired",{},true);
    const auto plan = take(graph.compile());
    std::optional<ComputeEncoder> encoder;
    const std::array callbacks{
        PassCallback{0,[](PassContext& pass,void* data) -> Result<void> {
            auto commands = pass.compute(); if (!commands) return std::unexpected(commands.error());
            static_cast<std::optional<ComputeEncoder>*>(data)->emplace(std::move(*commands)); return {};
        },&encoder},
        PassCallback{1,[](PassContext&,void* data) -> Result<void> {
            const auto result = static_cast<std::optional<ComputeEncoder>*>(data)->value().dispatch({1,1,1});
            require(!result && result.error().message.find("expired") != std::string::npos,"encoder survived the pass boundary"); return {};
        },&encoder}};
    auto execution = take(execute(plan,queue,{{},callbacks,{}}));
    require(take(queue.wait(execution.submission())),"encoder boundary execution did not complete");
}

void partial_contents(memory::ResourceHandle heap, SubmissionQueue& queue) {
    auto source = take(queue.create_buffer({16, vk::BufferUsageFlagBits::eTransferSrc, BufferMemory::upload}));
    std::array<std::uint32_t,4> expected{1,2,3,4}; check(source.write(0, std::as_bytes(std::span{expected})));
    auto graph = take(Graph::create(heap));
    const auto src = take(graph.declare_buffer("source", source.description(), Lifetime::external, true));
    const auto dst = take(graph.declare_buffer("parts", {16, vk::BufferUsageFlagBits::eTransferDst, BufferMemory::readback}));
    add(graph, "left", {read(src,0,8), write(dst,vk::PipelineStageFlagBits2::eCopy,0,8)});
    add(graph, "right", {read(src,8,8), write(dst,vk::PipelineStageFlagBits2::eCopy,8,8)});
    check(graph.mark_output(dst)); const auto plan = take(graph.compile());
    const std::array bindings{ExternalBinding{0, &source}};
    std::array callbacks{PassCallback{0, [](PassContext& pass, void*) { return pass.copy_buffer(0,1,8); }},
        PassCallback{1, [](PassContext& pass, void*) { return pass.copy_buffer(0,1,8,8,8); }}};
    const auto saved = callbacks[0].record;
    callbacks[0].record = [](PassContext& pass, void*) { return pass.copy_buffer(0,1,16); };
    require(!execute(plan, queue, {bindings,callbacks,{}}), "out-of-declaration byte range accepted");
    callbacks[0].record = saved;
    auto execution = take(execute(plan, queue, {bindings,callbacks,{}}));
    require(!execution.states().back().state.initialized, "partial writes incorrectly asserted whole-buffer initialization");
    require(take(queue.wait(execution.submission())), "partial graph wait failed");
    std::array<std::uint32_t,4> actual{}; check(take(execution.buffer(1))->read(0,std::as_writable_bytes(std::span{actual})));
    require(actual == expected, "partial producers lost content");
}

void budget_and_commit(memory::MemorySystem& system, SubmissionQueue& queue) {
    const auto heap = take(system.create_heap({"graph-budget", memory::DomainCategory::render, 32768}));
    {
        auto graph = take(Graph::create(heap));
        const auto id = take(graph.declare_buffer("result", {64, vk::BufferUsageFlagBits::eTransferDst, BufferMemory::readback}));
        add(graph, "fill", {write(id,vk::PipelineStageFlagBits2::eClear)}); check(graph.mark_output(id));
        const auto plan = take(graph.compile());
        const std::array callbacks{PassCallback{0,fill_first}};
        struct Block { void* pointer; std::size_t size; }; std::vector<Block> padding;
        while (auto block = heap.try_allocate(64)) padding.push_back({*block,64});
        while (auto block = heap.try_allocate(1)) padding.push_back({*block,1});
        bool failed = false, partial = false, succeeded = false;
        do {
            const auto before = heap.snapshot(); const auto submitted = queue.stats().submitted;
            {
                auto execution = execute(plan, queue, {{},callbacks,{},true});
                if (!execution) {
                    failed = true; partial |= heap.snapshot().allocation_count > before.allocation_count + 3;
                    require(queue.stats().submitted == submitted, "allocation failure published work");
                } else { succeeded = true; require(take(queue.wait(execution->submission())), "budget execution wait failed"); }
            }
            require(heap.snapshot().live_allocations == before.live_allocations &&
                heap.snapshot().backing_requested_bytes == before.backing_requested_bytes && allocations(queue) == 0 &&
                queue.stats().recording_slots == 0, "budget failure leaked a candidate");
            if (succeeded || padding.empty()) break;
            const auto block = padding.back(); padding.pop_back(); heap.deallocate(block.pointer,block.size);
        } while (true);
        for (const auto& block : padding) heap.deallocate(block.pointer,block.size);
        require(failed && partial && succeeded, "budget sweep missed required paths");
        // Close metadata resource inside native submission: successful publication must not allocate.
        close_at_submit = heap;
        auto execution = take(execute(plan, queue, {{},callbacks,{}}));
        close_at_submit = {};
        require(take(queue.wait(execution.submission())), "closed metadata heap broke successful commit");
        require(!execute(plan, queue, {{},callbacks,{}}), "closed plan Memory accepted execution");
        require(execution.states().size() == 1, "closed Memory invalidated exported snapshot");
    }
    require(heap.snapshot().live_allocations == 0 && heap.try_close().closed(), "execution metadata leaked");
}

void allocation_failure_after_first(memory::ResourceHandle heap, SubmissionQueue& queue) {
    auto graph = take(Graph::create(heap));
    const auto buffer = take(graph.declare_buffer("first allocation", {64}));
    const auto limit = queue.device().adapter().properties.limits.maxImageDimension2D;
    const auto image = take(graph.declare_image("unsupported extent", {limit + 1, 1}));
    auto image_write = image_use_for(image, true); image_write.access.state.stages = vk::PipelineStageFlagBits2::eClear;
    add(graph, "first", {write(buffer, vk::PipelineStageFlagBits2::eClear)}, true);
    add(graph, "second", {image_write}, true);
    const auto plan = take(graph.compile());
    const std::array callbacks{PassCallback{0,fill_first},PassCallback{1,noop}};
    const auto submitted = queue.stats().submitted;
    require(!execute(plan, queue, {{},callbacks,{}}), "unsupported allocation accepted");
    require(allocations(queue) == 0 && queue.stats().recording_slots == 0 && queue.stats().submitted == submitted,
        "partial GPU allocation failure leaked/published work");
}
}
int main(int argc, char** argv) {
    Diagnostics diagnostics;
    auto system = take(memory::MemorySystem::create());
    auto heap = take(system.create_heap({"graph-probe", memory::DomainCategory::render}));
    DeviceOptions options;
    options.validation = argc == 2 && std::strcmp(argv[1], "--validation") == 0 ? ValidationMode::required : ValidationMode::disabled;
    options.diagnostic_sink = diagnostic; options.diagnostic_user_data = &diagnostics;
    auto device = Device::create(heap, options);
    if (!device) { std::fprintf(stderr,"%s\n",device.error().message.c_str());
        return device.error().code == ErrorCode::not_found || device.error().code == ErrorCode::not_supported ? 77 : 1; }
    try {
        {
            native_submit = reinterpret_cast<PFN_vkQueueSubmit2>(device->device_proc("vkQueueSubmit2"));
            native_wait = reinterpret_cast<PFN_vkWaitSemaphores>(device->device_proc("vkWaitSemaphores"));
            auto queue = take(graphics::detail::SubmissionAccess::create(heap,std::move(*device),2,{submit_override,nullptr,wait_override}));
            const auto& info = queue.device().adapter();
            std::printf("GPU=%s driver=%s\n",info.properties.deviceName.data(),info.driver.driverInfo.data());
            roundtrip(heap,queue); require(allocations(queue) == 0,"roundtrip leak");
            import_and_failures(heap,queue,options); require(allocations(queue) == 0,"import/failure leak");
            subresources_and_hazards(heap,queue); require(allocations(queue) == 0,"subresource leak");
            imported_subresources(heap,queue); require(allocations(queue) == 0,"image import leak");
            encoder_boundary(heap,queue);
            partial_contents(heap,queue); require(allocations(queue) == 0,"partial contents leak");
            budget_and_commit(system,queue); require(allocations(queue) == 0,"budget/commit leak");
            allocation_failure_after_first(heap,queue);
            auto empty = take(Graph::create(heap)); const auto empty_plan = take(empty.compile());
            auto execution = take(execute(empty_plan,queue,{})); require(take(queue.wait(execution.submission())),"empty execution failed");
            require(!execute(CompiledGraph{},queue,{}),"empty owner plan accepted");
            submit_error = VK_ERROR_DEVICE_LOST;
            require(!execute(empty_plan,queue,{}),"device loss ignored");
            require(queue.stats().device_lost && !execute(empty_plan,queue,{}),"lost queue accepted work");
            require(!queue.close() && queue.stats().closed,"lost queue did not close");
            std::printf("graph roundtrips=4 pending=%u allocations=%u\n",queue.stats().pending_slots,allocations(queue));
        }
        device = std::unexpected(Error{ErrorCode::invalid_state,"released"});
        std::printf("errors=%u warnings=%u liveAllocations=%zu\n",diagnostics.errors.load(),diagnostics.warnings.load(),heap.snapshot().live_allocations);
        return diagnostics.errors == 0 && diagnostics.warnings == 0 && heap.snapshot().live_allocations == 0 && system.try_close().closed() ? 0 : 1;
    } catch (const std::exception& error) { std::fprintf(stderr,"graph probe failed: %s\n",error.what()); return 1; }
}
