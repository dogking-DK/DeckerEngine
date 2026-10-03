#include <dk/render/GpuAssets.hpp>
#include <dk/graphics/GraphExecution.hpp>
#include <dk/memory/SmartPtr.hpp>
#include <algorithm>
#include <array>
#include <new>

namespace dk::render {
using namespace graphics;
namespace detail {
struct GpuAssetState {
    explicit GpuAssetState(memory::ResourceHandle heap)
        : primitives(0, memory::Allocator<GpuPrimitive>{heap}), textures(0, memory::Allocator<GpuTexture>{heap}),
          materials(0, memory::Allocator<MaterialData>{heap}) {}
    AssetId id;
    std::uint64_t generation = 0;
    Submission submission;
    Vector<GpuPrimitive> primitives;
    Vector<GpuTexture> textures;
    Vector<MaterialData> materials;
};
struct GpuCacheState {
    explicit GpuCacheState(memory::ResourceHandle heap)
        : resource(heap), entries(0, memory::Allocator<std::shared_ptr<const GpuAssetState>>{heap}) {}
    memory::ResourceHandle resource;
    std::uint64_t generation = 0;
    Vector<std::shared_ptr<const GpuAssetState>> entries;
};
}
namespace {
template<class T> T take(Result<T> result) {
    if (!result) throw std::move(result.error());
    return std::move(*result);
}
void check(Result<void> result) { if (!result) throw std::move(result.error()); }
Error allocation_error() { return {ErrorCode::internal_error, "GPU asset allocation failed; cache unchanged"}; }
vk::SamplerAddressMode address(TextureWrap w) {
    switch (w) {
    case TextureWrap::clamp: return vk::SamplerAddressMode::eClampToEdge;
    case TextureWrap::mirrored_repeat: return vk::SamplerAddressMode::eMirroredRepeat;
    default: return vk::SamplerAddressMode::eRepeat;
    }
}
SamplerDesc sampler(const TextureSampler& input) {
    SamplerDesc result;
    const auto min = input.min_filter.value_or(TextureFilter::linear);
    result.min_filter = min == TextureFilter::nearest || min == TextureFilter::nearest_mipmap_nearest ||
        min == TextureFilter::nearest_mipmap_linear ? vk::Filter::eNearest : vk::Filter::eLinear;
    result.mag_filter = input.mag_filter == TextureFilter::nearest ? vk::Filter::eNearest : vk::Filter::eLinear;
    result.mipmap_mode = min == TextureFilter::nearest_mipmap_nearest || min == TextureFilter::linear_mipmap_nearest
        ? vk::SamplerMipmapMode::eNearest : vk::SamplerMipmapMode::eLinear;
    result.address_u = address(input.wrap_s); result.address_v = address(input.wrap_t);
    return result;
}
struct Copy { std::size_t source, destination; vk::DeviceSize bytes; std::uint32_t width = 0, height = 0; };
Result<void> copy_pass(graph::PassContext& pass, void* pointer) {
    for (const auto& copy : *static_cast<const Vector<Copy>*>(pointer)) {
        auto result = copy.width ? pass.copy_to_image(copy.source, copy.destination, {0,0,0,0,copy.width,copy.height})
                                 : pass.copy_buffer(copy.source, copy.destination, copy.bytes);
        if (!result) return result;
    }
    return {};
}
}
Result<GpuAssets> GpuAssets::create(memory::ResourceHandle heap) {
    if (!heap || heap.state() != memory::ResourceState::open)
        return std::unexpected(Error{ErrorCode::invalid_state, "GPU cache requires open memory"});
    try { return GpuAssets{memory::make_shared_in<detail::GpuCacheState>(heap, heap)}; }
    catch (const std::bad_alloc&) { return std::unexpected(allocation_error()); }
}
Result<GpuAsset> GpuAssets::upload(SubmissionQueue& queue, const CpuAsset& input) {
    if (!state_ || state_->resource.state() != memory::ResourceState::open)
        return std::unexpected(Error{ErrorCode::invalid_state, "GPU cache is empty or its memory is closing"});
    if (state_->generation == std::numeric_limits<std::uint64_t>::max())
        return std::unexpected(Error{ErrorCode::invalid_state, "GPU generation exhausted"});
    auto valid = validate_gpu_asset(input);
    if (!valid) return std::unexpected(valid.error());
    try {
        const auto heap = state_->resource;
        const auto old = std::ranges::find_if(state_->entries, [&](const auto& entry) { return entry->id == input.mesh.id; });
        const auto slot = static_cast<std::size_t>(old - state_->entries.begin());
        // Reserve before submit; publication below cannot allocate even if the heap starts closing.
        if (slot == state_->entries.size()) state_->entries.reserve(state_->entries.size() + 1);
        auto candidate = memory::make_shared_in<detail::GpuAssetState>(heap, heap);
        candidate->id = input.mesh.id; candidate->generation = state_->generation + 1;
        candidate->materials.assign(input.materials.begin(), input.materials.end());
        candidate->primitives.reserve(input.mesh.primitives.size());
        candidate->textures.reserve(input.textures.size());
        auto graph = take(graph::Graph::create(heap));
        Vector<Buffer> staging(0, memory::Allocator<Buffer>{heap});
        staging.reserve(input.mesh.primitives.size() * 2 + input.textures.size());
        Vector<graph::ExternalBinding> bindings(0, memory::Allocator<graph::ExternalBinding>{heap});
        Vector<graph::Use> uses(0, memory::Allocator<graph::Use>{heap});
        Vector<graph::FinalAccess> finals(0, memory::Allocator<graph::FinalAccess>{heap});
        Vector<Copy> copies(0, memory::Allocator<Copy>{heap});
        auto stage = [&](std::span<const std::byte> bytes) {
            const auto index = graph.counts().buffers;
            staging.push_back(take(queue.create_buffer({bytes.size(), vk::BufferUsageFlagBits::eTransferSrc, BufferMemory::upload})));
            auto& buffer = staging.back(); check(buffer.write(0, bytes));
            auto id = take(graph.declare_buffer("staging-" + std::to_string(index), buffer.description(), graph::Lifetime::external, true));
            uses.push_back({id, {{vk::PipelineStageFlagBits2::eCopy, vk::AccessFlagBits2::eTransferRead}}});
            bindings.push_back({index, &buffer, nullptr, {}});
            return index;
        };
        auto upload_buffer = [&](Buffer& buffer, std::span<const std::byte> bytes, bool vertex) {
            const auto source = stage(bytes);
            const auto destination = graph.counts().buffers;
            buffer = take(queue.create_buffer({bytes.size(), vk::BufferUsageFlagBits::eTransferDst | vk::BufferUsageFlagBits::eTransferSrc |
                (vertex ? vk::BufferUsageFlagBits::eVertexBuffer : vk::BufferUsageFlagBits::eIndexBuffer), BufferMemory::device}));
            const auto id = take(graph.declare_buffer("mesh-" + std::to_string(destination), buffer.description(), graph::Lifetime::external));
            uses.push_back({id, {{vk::PipelineStageFlagBits2::eCopy, vk::AccessFlagBits2::eTransferWrite}, 0, VK_WHOLE_SIZE, {}, true}});
            bindings.push_back({destination, &buffer, nullptr, {}});
            finals.push_back({destination, {{vertex ? vk::PipelineStageFlagBits2::eVertexAttributeInput : vk::PipelineStageFlagBits2::eIndexInput,
                vertex ? vk::AccessFlagBits2::eVertexAttributeRead : vk::AccessFlagBits2::eIndexRead}}});
            copies.push_back({source, destination, bytes.size()});
        };
        for (const auto& primitive : input.mesh.primitives) {
            candidate->primitives.emplace_back(); auto& output = candidate->primitives.back();
            output.vertex_count = static_cast<std::uint32_t>(primitive.positions.size());
            output.index_count = static_cast<std::uint32_t>(primitive.indices.size());
            output.has_normals = !primitive.normals.empty(); output.has_texcoords = !primitive.texcoords.empty();
            output.material = primitive.material;
            output.bounds_min = output.bounds_max = primitive.positions.front();
            Vector<GpuVertex> vertices(primitive.positions.size(), memory::Allocator<GpuVertex>{heap});
            for (std::size_t i = 0; i < vertices.size(); ++i) {
                const Vec3f normal = output.has_normals ? primitive.normals[i] : Vec3f{0,0,1};
                const Vec2f uv = output.has_texcoords ? primitive.texcoords[i] : Vec2f{0,0};
                auto& v = vertices[i];
                for (int c = 0; c < 3; ++c) { v.position[c] = primitive.positions[i][c]; v.normal[c] = normal[c]; }
                v.uv[0] = uv[0]; v.uv[1] = uv[1];
                output.bounds_min = output.bounds_min.cwiseMin(primitive.positions[i]);
                output.bounds_max = output.bounds_max.cwiseMax(primitive.positions[i]);
            }
            upload_buffer(output.vertices, std::as_bytes(std::span{vertices}), true);
            upload_buffer(output.indices, std::as_bytes(std::span{primitive.indices}), false);
        }
        const auto total_buffers = graph.counts().buffers + input.textures.size();
        for (const auto& texture : input.textures) {
            const auto source = stage(texture.rgba8);
            const auto destination = total_buffers + candidate->textures.size();
            candidate->textures.emplace_back(); auto& output = candidate->textures.back();
            output.id = texture.id; output.sampler_description = sampler(texture.sampler);
            output.image = take(queue.create_image({texture.width, texture.height, vk::Format::eR8G8B8A8Srgb}));
            output.view = take(queue.resources().create_view(output.image));
            output.sampler = take(queue.resources().create_sampler(output.sampler_description));
            const auto id = take(graph.declare_image("texture-" + std::to_string(destination), output.image.description(), graph::Lifetime::external));
            uses.push_back({id, {{vk::PipelineStageFlagBits2::eCopy, vk::AccessFlagBits2::eTransferWrite, vk::ImageLayout::eTransferDstOptimal},
                0, VK_WHOLE_SIZE, {vk::ImageAspectFlagBits::eColor,0,1,0,1}, true}});
            bindings.push_back({destination, nullptr, &output.image, {}});
            finals.push_back({destination, {{vk::PipelineStageFlagBits2::eFragmentShader, vk::AccessFlagBits2::eShaderSampledRead,
                vk::ImageLayout::eShaderReadOnlyOptimal}}});
            copies.push_back({source, destination, 0, texture.width, texture.height});
        }
        static_cast<void>(take(graph.add_pass({"upload GPU asset", uses, true})));
        const auto plan = take(graph.compile());
        const std::array callbacks{graph::PassCallback{0, copy_pass, &copies}};
        auto execution = take(graph::execute(plan, queue, {bindings, callbacks, finals}));
        candidate->submission = execution.submission();
        // The only observable cache commit. All following owner moves are allocation-free.
        if (slot == state_->entries.size()) state_->entries.push_back(candidate);
        else state_->entries[slot] = candidate;
        state_->generation = candidate->generation;
        return GpuAsset{std::move(candidate)};
    } catch (Error& error) { return std::unexpected(std::move(error)); }
    catch (const std::bad_alloc&) { return std::unexpected(allocation_error()); }
}
Result<GpuAsset> GpuAssets::find(AssetId id) const {
    if (state_) for (const auto& entry : state_->entries) if (entry->id == id) return GpuAsset{entry};
    return std::unexpected(Error{ErrorCode::not_found, "GPU asset is not cached"});
}
bool GpuAssets::unload(AssetId id) noexcept {
    if (!state_) return false;
    const auto entry = std::ranges::find_if(state_->entries, [&](const auto& value) { return value->id == id; });
    if (entry == state_->entries.end()) return false;
    state_->entries.erase(entry); return true;
}
void GpuAssets::clear() noexcept { if (state_) state_->entries.clear(); }
std::size_t GpuAssets::size() const noexcept { return state_ ? state_->entries.size() : 0; }
AssetId GpuAsset::id() const noexcept { return state_ ? state_->id : AssetId{}; }
std::uint64_t GpuAsset::generation() const noexcept { return state_ ? state_->generation : 0; }
Submission GpuAsset::submission() const noexcept { return state_ ? state_->submission : Submission{}; }
GpuMesh GpuAsset::mesh() const noexcept { return state_ ? GpuMesh{state_->id, state_->primitives} : GpuMesh{}; }
std::span<const GpuTexture> GpuAsset::textures() const noexcept { return state_ ? std::span<const GpuTexture>{state_->textures} : std::span<const GpuTexture>{}; }
std::span<const MaterialData> GpuAsset::materials() const noexcept { return state_ ? std::span<const MaterialData>{state_->materials} : std::span<const MaterialData>{}; }
Result<bool> GpuAsset::wait(SubmissionQueue& queue, std::uint64_t timeout) const {
    if (!state_) return std::unexpected(Error{ErrorCode::invalid_state, "empty GPU asset"});
    return queue.wait(state_->submission, timeout);
}
} // namespace dk::render
