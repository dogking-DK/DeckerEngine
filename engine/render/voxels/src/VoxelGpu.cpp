#include <dk/render/VoxelGpu.hpp>
#include <dk/graphics/GraphExecution.hpp>
#include <dk/graphics/ShaderCompiler.hpp>
#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>

namespace dk::render {
using namespace graphics;
namespace vx = geometry::voxels;
namespace detail {
struct VoxelQueryState {
    memory::ResourceHandle heap;
    ResourceFactory factory;
    Buffer identity;
    PipelineLayout layout;
    ComputePipeline pipeline;
    std::string reflection;
};
struct VoxelFrameState {
    graph::Execution execution;
    std::shared_ptr<const VoxelGpuGrid> grid;
    std::size_t count = 0;
};
}
namespace {
template<class T> T take(Result<T> value) { if (!value) throw value.error(); return std::move(*value); }
void check(Result<void> value) { if (!value) throw value.error(); }
Error invalid(const char* text) { return {ErrorCode::invalid_argument,text}; }
struct PackedRay { std::array<float,4> origin{}, direction{}; std::array<std::int32_t,4> coordinate{}; };
struct Parameters {
    std::array<std::int32_t,4> lower{}, upper{};
    std::array<std::uint32_t,4> control{};
    std::array<std::array<float,4>,3> normal_rows{};
};
static_assert(sizeof(PackedRay)==48 && sizeof(Parameters)==96 && sizeof(VoxelGpuResult)==64);
static_assert(offsetof(VoxelGpuResult,normal)==32 && offsetof(VoxelGpuResult,visited_regions)==48);
constexpr double index_limit = 1048576;
constexpr auto compute = vk::PipelineStageFlagBits2::eComputeShader;
graph::Use use(graph::ResourceId id,vk::PipelineStageFlags2 stage,vk::AccessFlags2 access,bool overwrite=false) {
    return {id,{{stage,access},0,VK_WHOLE_SIZE,{vk::ImageAspectFlagBits::eColor,0,1,0,1},overwrite}};
}
void owner(const detail::VoxelQueryState& state,SubmissionQueue& queue,const VoxelGpuGrid& grid) {
    if (state.heap.state()!=memory::ResourceState::open) throw Error{ErrorCode::invalid_state,"Voxel query memory is closing"};
    auto guard=take(queue.begin()); check(guard.retain(state.identity)); check(guard.retain(grid.buffer()));
}
void validate_count(SubmissionQueue& queue,std::size_t count,VoxelQueryLimits limits) {
    if (!count || count>1024*1024 || !limits.maximum_regions || limits.maximum_regions>1024*1024)
        throw invalid("Voxel query count or traversal limit is outside 1..1048576");
    const auto& device=queue.device().adapter().properties.limits;
    if ((count+63)/64>device.maxComputeWorkGroupCount[0] || count*sizeof(VoxelGpuResult)>device.maxStorageBufferRange ||
        count*sizeof(PackedRay)>device.maxStorageBufferRange ||
        sizeof(Parameters)+count*(sizeof(PackedRay)+2*sizeof(VoxelGpuResult))>limits.bytes)
        throw invalid("Voxel query exceeds device or payload byte limits");
}
Parameters parameters(const VoxelGpuGrid& grid,std::size_t count,std::uint32_t mode,VoxelQueryLimits limits) {
    Parameters p;
    for (int i=0;i<3;++i) { p.lower[i]=grid.source().domain().minimum[i]; p.upper[i]=grid.source().domain().maximum[i]; }
    p.control={static_cast<std::uint32_t>(count),static_cast<std::uint32_t>(grid.buffer().size()),limits.maximum_regions,mode};
    return p;
}
struct DispatchContext { const detail::VoxelQueryState& state; std::uint32_t count; };
Result<void> dispatch(graph::PassContext& pass,void* user) {
    try {
        const auto& c=*static_cast<DispatchContext*>(user);
        const std::array writes{BindingWrite{0,0,BufferBinding{take(pass.buffer(1))}}, BindingWrite{1,0,BufferBinding{take(pass.buffer(4))}},
            BindingWrite{2,0,BufferBinding{take(pass.buffer(2))}},BindingWrite{3,0,BufferBinding{take(pass.buffer(3))}}};
        auto bindings=take(c.state.factory.create_bindings(c.state.layout,0,writes));
        auto encoder=take(pass.compute()); check(encoder.bind_pipeline(c.state.pipeline));
        const std::array sets{&bindings}; check(encoder.bind_sets(sets)); return encoder.dispatch({(c.count+63)/64,1,1});
    } catch (const Error& error) { return std::unexpected(error); }
}
Result<void> copy(graph::PassContext& pass,void* user) {
    return pass.copy_buffer(3,0,static_cast<DispatchContext*>(user)->count*sizeof(VoxelGpuResult));
}
std::shared_ptr<detail::VoxelFrameState> execute(const detail::VoxelQueryState& state,SubmissionQueue& queue,
    std::shared_ptr<const VoxelGpuGrid> grid,const Parameters& params,std::span<const PackedRay> rays) {
    auto frame=std::make_shared<detail::VoxelFrameState>(); frame->grid=std::move(grid); frame->count=rays.size();
    auto uniform=take(state.factory.create_buffer({sizeof(params),vk::BufferUsageFlagBits::eUniformBuffer,BufferMemory::upload}));
    auto input=take(state.factory.create_buffer({rays.size_bytes(),vk::BufferUsageFlagBits::eStorageBuffer,BufferMemory::upload}));
    check(uniform.write(0,std::as_bytes(std::span{&params,1}))); check(input.write(0,std::as_bytes(rays)));
    auto g=take(graph::Graph::create(state.heap)); const auto bytes=rays.size()*sizeof(VoxelGpuResult);
    const auto readback=take(g.declare_buffer("voxel readback",{bytes,vk::BufferUsageFlagBits::eTransferDst,BufferMemory::readback}));
    const auto constants=take(g.declare_buffer("voxel parameters",uniform.description(),graph::Lifetime::external,true));
    const auto ray_data=take(g.declare_buffer("voxel rays",input.description(),graph::Lifetime::external,true));
    const auto output=take(g.declare_buffer("voxel hits",{bytes,vk::BufferUsageFlagBits::eStorageBuffer|vk::BufferUsageFlagBits::eTransferSrc}));
    const auto voxels=take(g.declare_buffer("NanoVDB",frame->grid->buffer().description(),graph::Lifetime::external,true));
    const std::array uses{use(constants,compute,vk::AccessFlagBits2::eUniformRead),use(ray_data,compute,vk::AccessFlagBits2::eShaderStorageRead),
        use(voxels,compute,vk::AccessFlagBits2::eShaderStorageRead),use(output,compute,vk::AccessFlagBits2::eShaderStorageWrite,true)};
    static_cast<void>(take(g.add_pass({"voxel shared HDDA",uses})));
    const std::array copies{use(output,vk::PipelineStageFlagBits2::eCopy,vk::AccessFlagBits2::eTransferRead),
        use(readback,vk::PipelineStageFlagBits2::eCopy,vk::AccessFlagBits2::eTransferWrite,true)};
    static_cast<void>(take(g.add_pass({"voxel readback",copies}))); check(g.mark_output(readback));
    auto plan=take(g.compile()); DispatchContext context{state,static_cast<std::uint32_t>(rays.size())};
    const std::array bindings{graph::ExternalBinding{1,&uniform},graph::ExternalBinding{2,&input},graph::ExternalBinding{4,&frame->grid->buffer()}};
    const std::array callbacks{graph::PassCallback{0,dispatch,&context},graph::PassCallback{1,copy,&context}};
    const std::array finals{graph::FinalAccess{0,{{vk::PipelineStageFlagBits2::eHost,vk::AccessFlagBits2::eHostRead}}}};
    frame->execution=take(graph::execute(plan,queue,{bindings,callbacks,finals}));
    return frame;
}
}
Result<std::unique_ptr<VoxelGpuCache>> VoxelGpuCache::create(SubmissionQueue& queue,VoxelGpuLimits limits) {
    if (!limits.bytes || limits.bytes>2ull*1024*1024*1024 || !limits.generations || limits.generations>2)
        return std::unexpected(invalid("Voxel cache supports positive byte budget up to 2 GiB and 1..2 generations"));
    auto identity=queue.create_buffer({4}); if (!identity) return std::unexpected(identity.error());
    return std::unique_ptr<VoxelGpuCache>(new VoxelGpuCache(std::move(*identity),limits));
}
Result<std::shared_ptr<const VoxelGpuGrid>> VoxelGpuCache::prepare(SubmissionQueue& queue,const vx::SparseGrid& source) {
    try {
        check(queue.poll()); auto batch=take(queue.begin()); check(batch.retain(identity_));
        const auto bytes=source.bytes().size();
        if (std::endian::native!=std::endian::little || !bytes || bytes%32 || bytes>UINT32_MAX ||
            bytes>queue.device().adapter().properties.limits.maxStorageBufferRange)
            throw invalid("NanoVDB buffer exceeds little-endian/alignment/address/device contract");
        for (int axis=0;axis<3;++axis)
            if (source.domain().minimum[axis]<-index_limit || source.domain().maximum[axis]>index_limit)
                throw invalid("GPU voxel domain exceeds +/-2^20 float traversal range");
        if (!queue.stats().pending_slots && queue.stats().recording_slots==1)
            std::erase_if(live_,[&](const auto& live){ return live!=current_ && live.use_count()==1; });
        if (current_ && current_->source().bytes().data()==source.bytes().data()) return current_;
        std::uint64_t occupied=2*bytes;
        const auto completed=queue.stats().completed;
        for (const auto& live:live_) {
            occupied+=live->buffer().size();
            if (live->upload_submission().value()>completed) occupied+=live->buffer().size();
        }
        if (occupied>limits_.bytes || live_.size()+1>limits_.generations)
            throw Error{ErrorCode::invalid_state,"Voxel live generation or upload byte budget exceeded"};
        if (current_ && current_->generation()==UINT64_MAX) throw Error{ErrorCode::invalid_state,"Voxel generation exhausted"};
        auto candidate=std::shared_ptr<VoxelGpuGrid>(new VoxelGpuGrid(source));
        candidate->generation_=current_ ? current_->generation()+1 : 1;
        live_.reserve(live_.size()+1);
        candidate->buffer_=take(queue.create_buffer({bytes,vk::BufferUsageFlagBits::eStorageBuffer|vk::BufferUsageFlagBits::eTransferDst|vk::BufferUsageFlagBits::eTransferSrc}));
        check(batch.upload(candidate->buffer_,source.bytes()));
        candidate->upload_=take(queue.submit(std::move(batch)));
        live_.push_back(candidate); current_=std::move(candidate); return current_;
    } catch (const Error& error) { return std::unexpected(error); }
    catch (const std::bad_alloc&) { return std::unexpected(Error{ErrorCode::internal_error,"Voxel upload allocation failed"}); }
}
Result<VoxelGpuQuery> VoxelGpuQuery::create(memory::ResourceHandle heap,SubmissionQueue& queue,
    const std::filesystem::path& directory,const std::filesystem::path& includes) {
    if (!heap || heap.state()!=memory::ResourceState::open) return std::unexpected(Error{ErrorCode::invalid_state,"Voxel query requires open memory"});
    try {
        auto state=memory::make_shared_in<detail::VoxelQueryState>(heap); state->heap=heap; state->factory=queue.resources();
        state->identity=take(state->factory.create_buffer({4}));
        const std::array include_paths{includes.empty() ? std::filesystem::path{DK_VOXEL_NANO_INCLUDE_DIR} : includes};
        auto shader=take(compile_shader({(directory.empty() ? std::filesystem::path{DK_VOXEL_SHADER_DIR} : directory)/"voxel-query.slang",
            "computeMain",ShaderStage::compute,include_paths,{},true},heap));
        if (shader.thread_group_size!=std::array<std::uint32_t,3>{64,1,1} || shader.bindings.size()!=4 || !shader.push_constants.empty())
            throw invalid("Unexpected voxel compute reflection");
        for (std::size_t i=0;i<4;++i)
            if (shader.bindings[i].set!=0 || shader.bindings[i].binding!=i || shader.bindings[i].count!=1 ||
                shader.bindings[i].type!=(i==0 ? ShaderDescriptorType::uniform_buffer : ShaderDescriptorType::storage_buffer) ||
                (i==0 && shader.bindings[i].block_size!=sizeof(Parameters))) throw invalid("Voxel descriptor ABI mismatch");
        const auto json=shader_reflection_json(shader); state->reflection.assign(json.data(),json.size());
        auto module=take(state->factory.create_shader(shader)); const std::array shaders{&module};
        state->layout=take(state->factory.create_pipeline_layout(shaders));
        state->pipeline=take(state->factory.create_compute_pipeline({&module,&state->layout}));
        return VoxelGpuQuery{std::move(state)};
    } catch (const Error& error) { return std::unexpected(error); }
    catch (const std::bad_alloc&) { return std::unexpected(Error{ErrorCode::internal_error,"Voxel query allocation failed"}); }
}
Result<VoxelGpuFrame> VoxelGpuQuery::trace(SubmissionQueue& queue,std::shared_ptr<const VoxelGpuGrid> grid,
    std::span<const geometry::Ray> rays,const Transformd& local_to_world,VoxelQueryLimits limits) const {
    if (!state_ || !grid) return std::unexpected(invalid("Voxel trace requires renderer and grid"));
    try {
        owner(*state_,queue,*grid); validate_count(queue,rays.size(),limits);
        const auto world=take(local_to_world.compose(grid->source().corner_to_local())); const auto inverse=take(world.inverse());
        auto p=parameters(*grid,rays.size(),0,limits);
        Mat3d normal=inverse.matrix().block<3,3>(0,0).transpose(); normal/=normal.cwiseAbs().maxCoeff();
        for (int row=0;row<3;++row) for (int col=0;col<3;++col) p.normal_rows[row][col]=static_cast<float>(normal(row,col));
        for (int col=0;col<3;++col)
            if (p.normal_rows[0][col]==0 && p.normal_rows[1][col]==0 && p.normal_rows[2][col]==0) throw invalid("GPU voxel normal loses precision");
        std::vector<PackedRay> input; input.reserve(rays.size());
        for (const auto& ray:rays) {
            check(geometry::validate(ray)); const auto o=take(inverse.transform_point(ray.origin)), d=take(inverse.transform_direction(ray.direction));
            PackedRay packed;
            for (int axis=0;axis<3;++axis) {
                if (std::abs(o[axis])>index_limit || std::abs(d[axis])>index_limit || (d[axis]!=0 && std::abs(d[axis])<1e-8))
                    throw invalid("GPU voxel ray exceeds supported index-space precision range");
                packed.origin[axis]=static_cast<float>(o[axis]); packed.direction[axis]=static_cast<float>(d[axis]);
            }
            if (ray.minimum>std::numeric_limits<float>::max())
                throw invalid("GPU voxel t range is not representable");
            packed.origin[3]=static_cast<float>(ray.minimum);
            if (ray.minimum>0 && packed.origin[3]==0) throw invalid("GPU voxel near bound underflows float");
            // Within the admitted origin/domain/direction range, finite slab crossings
            // are <= 2.1e14. Preserve the CPU default DBL_MAX as an unbounded far end.
            packed.direction[3]=static_cast<float>(std::min(ray.maximum,static_cast<double>(std::numeric_limits<float>::max())));
            input.push_back(packed);
        }
        return VoxelGpuFrame{execute(*state_,queue,std::move(grid),p,input)};
    } catch (const Error& error) { return std::unexpected(error); }
    catch (const std::bad_alloc&) { return std::unexpected(Error{ErrorCode::internal_error,"Voxel trace allocation failed"}); }
}
Result<VoxelGpuFrame> VoxelGpuQuery::sample(SubmissionQueue& queue,std::shared_ptr<const VoxelGpuGrid> grid,
    std::span<const vx::Coord> coords,VoxelQueryLimits limits) const {
    if (!state_ || !grid) return std::unexpected(invalid("Voxel sample requires renderer and grid"));
    try {
        owner(*state_,queue,*grid); validate_count(queue,coords.size(),limits);
        const auto p=parameters(*grid,coords.size(),1,limits);
        std::vector<PackedRay> input(coords.size());
        for (std::size_t i=0;i<coords.size();++i) for (int axis=0;axis<3;++axis) {
            if (std::abs(static_cast<double>(coords[i][axis]))>index_limit) throw invalid("GPU sample coordinate outside +/-2^20");
            input[i].coordinate[axis]=coords[i][axis];
        }
        return VoxelGpuFrame{execute(*state_,queue,std::move(grid),p,input)};
    } catch (const Error& error) { return std::unexpected(error); }
    catch (const std::bad_alloc&) { return std::unexpected(Error{ErrorCode::internal_error,"Voxel sample allocation failed"}); }
}
std::string_view VoxelGpuQuery::shader_reflection() const { return state_ ? state_->reflection : std::string_view{}; }
Submission VoxelGpuFrame::submission() const { return state_ ? state_->execution.submission() : Submission{}; }
Result<bool> VoxelGpuFrame::read(SubmissionQueue& queue,std::vector<VoxelGpuResult>& output,std::uint64_t timeout) const {
    if (!state_) return std::unexpected(invalid("Empty voxel frame"));
    try {
        if (!take(queue.wait(submission(),timeout))) return false;
        std::vector<VoxelGpuResult> candidate(state_->count);
        check(take(state_->execution.buffer(0))->read(0,std::as_writable_bytes(std::span{candidate})));
        for (std::size_t i=0;i<candidate.size();++i) {
            const auto& r=candidate[i];
            if (r.status>VoxelGpuStatus::sample) throw Error{ErrorCode::invalid_argument,"Voxel GPU query failed at ray " + std::to_string(i) + " with status " + std::to_string(static_cast<unsigned>(r.status))};
            if (r.status==VoxelGpuStatus::hit && (!r.value || r.value>INT32_MAX || r.face>=6 || !std::isfinite(r.distance) ||
                !std::ranges::all_of(r.normal,[](float v){ return std::isfinite(v); }))) throw Error{ErrorCode::internal_error,"Invalid voxel GPU hit"};
        }
        output=std::move(candidate); return true;
    } catch (const Error& error) { return std::unexpected(error); }
    catch (const std::bad_alloc&) { return std::unexpected(Error{ErrorCode::internal_error,"Voxel readback allocation failed"}); }
}
} // namespace dk::render
