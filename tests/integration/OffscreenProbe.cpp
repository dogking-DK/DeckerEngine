#include <dk/graphics/Offscreen.hpp>
#include <dk/graphics/CommandEncoder.hpp>
#include <dk/memory/MemorySystem.hpp>
#include "OffscreenPolicy.hpp"
#include "SubmissionInternal.hpp"
#include "ObjectInternal.hpp"
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
void graph_pipeline(memory::ResourceHandle,const DeviceOptions&,const std::array<std::uint32_t,1024>&);
namespace {
void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
void take(Result<void>&& result) { if (!result) throw std::runtime_error(result.error().message); }
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
    const auto allocation_failure = executor.draw(vertex, fragment);
    require(!allocation_failure && allocation_failure.error().code == ErrorCode::internal_error &&
        executor.stats().submitted == submitted && allocations(executor) == 0,
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
void pipeline_bindings(memory::ResourceHandle resource, const DeviceOptions& options,
    const CompiledShader& vertex, const CompiledShader& fragment)
{
    auto queue = take(SubmissionQueue::create(resource, take(Device::create(resource, options))));
    auto factory = queue.resources();
    const auto source = std::filesystem::path{DK_COMMON_SHADER_DIR}.parent_path().parent_path() / "tests/fixtures/shaders/usage-bindings.slang";
    auto artifact = take(compile_shader({source, "computeMain", ShaderStage::compute}, resource));
    auto shader = take(factory.create_shader(artifact));
    const std::array shader_list{&shader};
    auto layout = take(factory.create_pipeline_layout(shader_list));
    auto equivalent = take(factory.create_pipeline_layout(shader_list));
    require(layout.compatible_for_set(equivalent, 2), "equivalent layout rejected");
    auto pipeline = take(factory.create_compute_pipeline({&shader, &layout}));
    auto repeated = take(factory.create_compute_pipeline({&shader, &layout}));
    auto vs = take(factory.create_shader(vertex));
    auto fs = take(factory.create_shader(fragment));
    const std::array graphics_shaders{&vs, &fs};
    auto graphics_layout = take(factory.create_pipeline_layout(graphics_shaders));
    auto graphics = take(factory.create_graphics_pipeline({&vs, &fs, &graphics_layout}));
    require(!factory.create_compute_pipeline({&shader, &graphics_layout}), "incomplete pipeline layout accepted");
    require(!factory.create_compute_pipeline({&vs, &layout}), "wrong pipeline stage accepted");

    auto sampled = take(factory.create_image({4, 4}));
    auto output_image = take(factory.create_image({4, 4, vk::Format::eR8G8B8A8Unorm,
        vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eTransferSrc}));
    auto sampled_view = take(factory.create_view(sampled));
    auto output_view = take(factory.create_view(output_image));
    auto sampler = take(factory.create_sampler());
    auto uniform = take(factory.create_buffer({16, vk::BufferUsageFlagBits::eUniformBuffer, BufferMemory::upload}));
    auto storage = take(factory.create_buffer({64, vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eTransferSrc}));
    const std::array<float,4> scale{1,1,1,1};
    take(uniform.write(0, std::as_bytes(std::span{scale})));
    const std::array<BindingWrite,4> set1_writes{{
        {0, 0, ImageBinding{&sampled_view}}, {0, 1, ImageBinding{&sampled_view}},
        {0, 2, ImageBinding{&sampled_view}}, {3, 0, SamplerBinding{&sampler}}}};
    const std::array<BindingWrite,3> set2_writes{{
        {0, 0, BufferBinding{&storage}}, {1, 0, ImageBinding{&output_view, vk::ImageLayout::eGeneral}},
        {4, 0, BufferBinding{&uniform}}}};
    auto bindings1 = take(factory.create_bindings(layout, 1, set1_writes));
    auto bindings2 = take(factory.create_bindings(layout, 2, set2_writes));
    auto replacement = take(factory.create_bindings(layout, 2, set2_writes));
    require(bindings2.handle() != replacement.handle(), "immutable replacement reused live descriptor set");
    // A lifetime reference from bindings does not make uniform bytes busy.
    take(uniform.write(0, std::as_bytes(std::span{scale})));
    require(!factory.create_bindings(layout, 1, std::span{set1_writes}.first(3)), "missing array element accepted");
    auto duplicate = set1_writes;
    duplicate[1] = duplicate[0];
    require(!factory.create_bindings(layout, 1, duplicate), "duplicate array element accepted");
    auto invalid = set2_writes;
    invalid[2].resource = BufferBinding{&uniform, 1, 15};
    require(!factory.create_bindings(layout, 2, invalid), "misaligned uniform range accepted");
    graphics::detail::ObjectAccess::fail_creation(factory, VK_ERROR_OUT_OF_HOST_MEMORY);
    require(!factory.create_bindings(layout, 2, set2_writes), "descriptor post-create failure ignored");
    // Allocate across a page boundary, then free and reuse independently of frame numbers.
    Vector<BindingSet> many{memory::Allocator<BindingSet>{resource}};
    for (unsigned i = 0; i < 40; ++i) many.push_back(take(factory.create_bindings(layout, 2, set2_writes)));
    many.clear();
    bool exhausted = false;
    for (unsigned i = 0; i < 2048; ++i) {
        auto next = factory.create_bindings(layout, 2, set2_writes);
        if (!next) {
            require(next.error().code == ErrorCode::conflict, "descriptor capacity returned unexpected failure");
            exhausted = true;
            break;
        }
        many.push_back(std::move(*next));
    }
    require(exhausted, "descriptor pool directory was unbounded");
    many.clear();
    auto recovered = take(factory.create_bindings(layout, 2, set2_writes));

    std::array<std::uint32_t,16> red{};
    red.fill(0xff0000ffu);
    auto upload = take(factory.create_buffer({64, vk::BufferUsageFlagBits::eTransferSrc, BufferMemory::upload}));
    auto readback = take(factory.create_buffer({64, vk::BufferUsageFlagBits::eTransferDst, BufferMemory::readback}));
    auto image_readback = take(factory.create_buffer({64, vk::BufferUsageFlagBits::eTransferDst, BufferMemory::readback}));
    take(upload.write(0, std::as_bytes(std::span{red})));
    for (unsigned repeat = 0; repeat < 2; ++repeat) {
        auto batch = take(queue.begin());
        const std::array upload_uses{buffer_use(upload, vk::PipelineStageFlagBits2::eCopy, vk::AccessFlagBits2::eTransferRead),
            image_use(sampled, vk::PipelineStageFlagBits2::eCopy, vk::AccessFlagBits2::eTransferWrite, vk::ImageLayout::eTransferDstOptimal)};
        take(batch.prepare(upload_uses));
        take(batch.copy_to_image(upload, sampled, {0,0,0,0,4,4}));
        auto output_use = image_use(output_image, vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderStorageWrite, vk::ImageLayout::eGeneral);
        output_use.full_overwrite = true;
        const std::array compute_uses{
            image_use(sampled, vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderSampledRead, vk::ImageLayout::eShaderReadOnlyOptimal),
            output_use, buffer_use(uniform, vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eUniformRead),
            buffer_use(storage, vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderStorageWrite)};
        auto compute = take(batch.compute());
        take(compute.bind_pipeline(pipeline));
        const std::array sets{&bindings1,&bindings2};
        take(compute.bind_sets(sets));
        require(!compute.dispatch({4,4,1}), "dispatch accepted undeclared binding access");
        require(!uniform.write(0, std::as_bytes(std::span{scale})), "bound resource closure did not block host writes");
        take(batch.prepare(compute_uses));
        take(compute.dispatch({4,4,1}));
        require(!compute.dispatch({4,4,1}), "same-layout WAW dispatch accepted without barrier");
        take(batch.prepare(compute_uses));
        take(compute.bind_pipeline(repeated));
        take(compute.dispatch({4,4,1}));
        const std::array read_uses{buffer_use(storage,vk::PipelineStageFlagBits2::eCopy,vk::AccessFlagBits2::eTransferRead),
            buffer_use(readback,vk::PipelineStageFlagBits2::eCopy,vk::AccessFlagBits2::eTransferWrite),
            image_use(output_image,vk::PipelineStageFlagBits2::eCopy,vk::AccessFlagBits2::eTransferRead,vk::ImageLayout::eTransferSrcOptimal),
            buffer_use(image_readback,vk::PipelineStageFlagBits2::eCopy,vk::AccessFlagBits2::eTransferWrite)};
        take(batch.prepare(read_uses));
        take(batch.copy_buffer(storage,readback,64));
        take(batch.copy_to_buffer(output_image,image_readback,{0,0,0,0,4,4}));
        const std::array host{buffer_use(readback,vk::PipelineStageFlagBits2::eHost,vk::AccessFlagBits2::eHostRead),
            buffer_use(image_readback,vk::PipelineStageFlagBits2::eHost,vk::AccessFlagBits2::eHostRead)};
        take(batch.prepare(host));
        auto ticket = take(queue.submit(std::move(batch)));
        require(!compute.dispatch({4,4,1}), "submitted encoder remained valid");
        require(take(queue.wait(ticket)), "pipeline binding dispatch did not complete");
        std::array<float,16> floats{};
        std::array<std::uint32_t,16> pixels{};
        take(readback.read(0, std::as_writable_bytes(std::span{floats})));
        take(image_readback.read(0, std::as_writable_bytes(std::span{pixels})));
        for (auto value : floats) require(value == 1, "multi-set buffer result mismatch");
        require(pixels == red, "sampled/storage image result mismatch");
    }
    shader = {};
    layout = {};
    require(bool(pipeline.handle()) && bool(bindings2.handle()), "pipeline/binding lost owned layout");
    take(queue.close());
}
std::array<std::uint32_t,1024> indexed_depth(memory::ResourceHandle resource, const DeviceOptions& options)
{
    auto queue = take(SubmissionQueue::create(resource, take(Device::create(resource, options))));
    auto factory = queue.resources();
    const auto source = std::filesystem::path{DK_COMMON_SHADER_DIR}.parent_path().parent_path() / "tests/fixtures/shaders/usage-geometry.slang";
    const std::array<ShaderDefine,1> compute_defines{{{"DK_GEOMETRY_COMPUTE","1"}}}, fragment_defines{{{"DK_GEOMETRY_FRAGMENT","1"}}};
    auto cs = take(factory.create_shader(take(compile_shader({source,"computeMain",ShaderStage::compute,{},compute_defines},resource))));
    auto vs = take(factory.create_shader(take(compile_shader({source,"vertexMain",ShaderStage::vertex},resource))));
    auto fs = take(factory.create_shader(take(compile_shader({source,"fragmentMain",ShaderStage::fragment,{},fragment_defines},resource))));
    const std::array compute_shaders{&cs};
    const std::array render_shaders{&vs,&fs};
    auto cl = take(factory.create_pipeline_layout(compute_shaders));
    auto gl = take(factory.create_pipeline_layout(render_shaders));
    auto cp = take(factory.create_compute_pipeline({&cs,&cl}));
    const std::array vertex_bindings{vk::VertexInputBindingDescription{0,16,vk::VertexInputRate::eVertex}};
    const std::array attributes{vk::VertexInputAttributeDescription{0,0,vk::Format::eR32G32B32A32Sfloat,0}};
    GraphicsPipelineDesc desc{&vs,&fs,&gl,vertex_bindings,attributes};
    desc.depth_format = vk::Format::eD32Sfloat;
    desc.depth_test = desc.depth_write = true;
    auto gp = take(factory.create_graphics_pipeline(desc));
    auto vertices = take(factory.create_buffer({48,vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eVertexBuffer}));
    auto indices = take(factory.create_buffer({12,vk::BufferUsageFlagBits::eIndexBuffer,BufferMemory::upload}));
    const std::array<std::uint32_t,3> index_data{0,1,2};
    take(indices.write(0,std::as_bytes(std::span{index_data})));
    const std::array<BindingWrite,1> writes{{{0,0,BufferBinding{&vertices}}}};
    auto bindings = take(factory.create_bindings(cl,0,writes));
    auto color = take(factory.create_image({32,32,vk::Format::eR8G8B8A8Unorm,vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eTransferSrc}));
    auto depth = take(factory.create_image({32,32,vk::Format::eD32Sfloat,vk::ImageUsageFlagBits::eDepthStencilAttachment}));
    auto cv = take(factory.create_view(color));
    ImageViewDesc dv_desc{};
    dv_desc.range.aspectMask = vk::ImageAspectFlagBits::eDepth;
    auto dv = take(factory.create_view(depth,dv_desc));
    auto readback = take(factory.create_buffer({32*32*4,vk::BufferUsageFlagBits::eTransferDst,BufferMemory::readback}));
    auto batch = take(queue.begin());
    auto compute = take(batch.compute());
    take(compute.bind_pipeline(cp));
    const std::array sets{&bindings};
    take(compute.bind_sets(sets));
    const std::array compute_use{buffer_use(vertices,vk::PipelineStageFlagBits2::eComputeShader,vk::AccessFlagBits2::eShaderStorageWrite)};
    take(batch.prepare(compute_use));
    take(compute.dispatch({3,1,1}));
    const std::array render_uses{
        buffer_use(vertices,vk::PipelineStageFlagBits2::eVertexAttributeInput,vk::AccessFlagBits2::eVertexAttributeRead),
        buffer_use(indices,vk::PipelineStageFlagBits2::eIndexInput,vk::AccessFlagBits2::eIndexRead),
        image_use(color,vk::PipelineStageFlagBits2::eColorAttachmentOutput,vk::AccessFlagBits2::eColorAttachmentRead | vk::AccessFlagBits2::eColorAttachmentWrite,vk::ImageLayout::eColorAttachmentOptimal),
        image_use(depth,vk::PipelineStageFlagBits2::eEarlyFragmentTests | vk::PipelineStageFlagBits2::eLateFragmentTests,
            vk::AccessFlagBits2::eDepthStencilAttachmentRead | vk::AccessFlagBits2::eDepthStencilAttachmentWrite,vk::ImageLayout::eDepthStencilAttachmentOptimal,dv_desc.range)};
    take(batch.prepare(render_uses));
    RenderingDesc rendering{};
    rendering.color.view = &cv;
    rendering.depth.view = &dv;
    auto render = take(batch.begin_rendering(rendering));
    require(!compute.dispatch({3,1,1}) && !batch.compute() && !batch.prepare(render_uses), "rendering allowed invalid scope operations");
    require(!queue.submit(std::move(batch)), "unclosed rendering submitted");
    take(render.bind_pipeline(gp));
    take(render.vertex_buffer(0,vertices));
    take(render.index_buffer(indices,vk::IndexType::eUint32));
    require(!render.draw_indexed(3), "unset push bytes accepted");
    const std::array<float,4> red{1,0,0,1}, green{0,1,0,1};
    take(render.push_constants(vk::ShaderStageFlagBits::eFragment,0,std::as_bytes(std::span{red})));
    require(!render.draw_indexed(4), "out-of-range indices accepted");
    require(!render.draw(4), "out-of-range vertices accepted");
    take(render.draw_indexed(3));
    take(render.push_constants(vk::ShaderStageFlagBits::eFragment,0,std::as_bytes(std::span{green})));
    take(render.draw_indexed(3)); // Equal depth fails Less; output must remain red.
    take(render.end());
    require(!render.draw_indexed(3), "ended encoder accepted draw");
    const std::array copy_use{image_use(color,vk::PipelineStageFlagBits2::eCopy,vk::AccessFlagBits2::eTransferRead,vk::ImageLayout::eTransferSrcOptimal),
        buffer_use(readback,vk::PipelineStageFlagBits2::eCopy,vk::AccessFlagBits2::eTransferWrite)};
    take(batch.prepare(copy_use));
    take(batch.copy_to_buffer(color,readback,{0,0,0,0,32,32}));
    const std::array host{buffer_use(readback,vk::PipelineStageFlagBits2::eHost,vk::AccessFlagBits2::eHostRead)};
    take(batch.prepare(host));
    // Only the batch retains pipelines, views, descriptor sets and their transitive owners.
    cp = {}; gp = {}; cl = {}; gl = {}; cs = {}; vs = {}; fs = {}; bindings = {}; cv = {}; dv = {};
    vertices = {}; indices = {}; color = {}; depth = {};
    auto ticket = take(queue.submit(std::move(batch)));
    require(take(queue.wait(ticket)), "indexed depth draw timed out");
    std::array<std::uint32_t,32*32> pixels{};
    take(readback.read(0,std::as_writable_bytes(std::span{pixels})));
    require(pixels[16*32+16] == 0xff0000ffu && pixels[0] == 0xff000000u, "indexed depth test/clear result mismatch");
    take(queue.close());
    return pixels;
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
            pipeline_bindings(resource, options, vertex, fragment);
            graph_pipeline(resource,options,indexed_depth(resource, options));
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
