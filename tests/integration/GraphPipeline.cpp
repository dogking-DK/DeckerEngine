#include <dk/graphics/GraphExecution.hpp>
#include <dk/graphics/GraphDiagnostics.hpp>
#include <dk/graphics/ShaderCompiler.hpp>
#include <array>
#include <fstream>
#include <stdexcept>
using namespace dk;
using namespace dk::graphics;
using namespace dk::graphics::graph;
namespace {
void require(bool condition,const char* message) { if (!condition) throw std::runtime_error(message); }
template<class T> T take(Result<T> result) {
    if (!result) {
        auto message = result.error().message;
        for (const auto& context : result.error().context) message += " | " + context;
        throw std::runtime_error(message);
    }
    return std::move(*result);
}
void check(Result<void> result) { if (!result) throw std::runtime_error(result.error().message); }
Use access(ResourceId id,vk::PipelineStageFlags2 stage,vk::AccessFlags2 flags,bool overwrite=false,
    vk::ImageLayout layout=vk::ImageLayout::eUndefined) {
    return {id,{{stage,flags,layout},0,VK_WHOLE_SIZE,{vk::ImageAspectFlagBits::eColor,0,1,0,1},overwrite}};
}
void add(Graph& graph,std::string_view name,std::initializer_list<Use> uses) {
    static_cast<void>(take(graph.add_pass({name,{uses.begin(),uses.size()}})));
}
}
// One repeatable public-API example: upload -> compute -> clear/draw -> readback.
void graph_pipeline(memory::ResourceHandle heap,const DeviceOptions& options,const std::array<std::uint32_t,1024>& reference) {
    auto queue = take(SubmissionQueue::create(heap,take(Device::create(heap,options))));
    auto factory = queue.resources();
    const auto common = std::filesystem::path{DK_COMMON_SHADER_DIR};
    const auto geometry = common.parent_path().parent_path()/"tests/fixtures/shaders/usage-geometry.slang";
    const std::array<ShaderDefine,1> fragment_defines{{{"DK_GEOMETRY_FRAGMENT","1"}}};
    auto cs = take(factory.create_shader(take(compile_shader({common/"graph-transform.slang","computeMain",ShaderStage::compute},heap))));
    auto vs = take(factory.create_shader(take(compile_shader({geometry,"vertexMain",ShaderStage::vertex},heap))));
    auto fs = take(factory.create_shader(take(compile_shader({geometry,"fragmentMain",ShaderStage::fragment,{},fragment_defines},heap))));
    const std::array cs_list{&cs}; const std::array gs_list{&vs,&fs};
    auto cl = take(factory.create_pipeline_layout(cs_list)); auto gl = take(factory.create_pipeline_layout(gs_list));
    auto cp = take(factory.create_compute_pipeline({&cs,&cl}));
    const std::array vb{vk::VertexInputBindingDescription{0,16,vk::VertexInputRate::eVertex}};
    const std::array va{vk::VertexInputAttributeDescription{0,0,vk::Format::eR32G32B32A32Sfloat,0}};
    auto gp = take(factory.create_graphics_pipeline({&vs,&fs,&gl,vb,va}));
    auto vertices = take(factory.create_buffer({48,vk::BufferUsageFlagBits::eTransferSrc,BufferMemory::upload}));
    auto indices = take(factory.create_buffer({12,vk::BufferUsageFlagBits::eTransferSrc,BufferMemory::upload}));
    const std::array<float,12> vertex_data{-1,-1,0.25f,1, 1,-1,0.25f,1, 0,1,0.25f,1};
    const std::array<std::uint32_t,3> index_data{0,1,2};
    check(vertices.write(0,std::as_bytes(std::span{vertex_data}))); check(indices.write(0,std::as_bytes(std::span{index_data})));
    auto graph = take(Graph::create(heap));
    const auto upload_v = take(graph.declare_buffer("vertex-upload",vertices.description(),Lifetime::external,true));
    const auto v = take(graph.declare_buffer("vertices",{48,vk::BufferUsageFlagBits::eTransferDst | vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eVertexBuffer}));
    const auto upload_i = take(graph.declare_buffer("index-upload",indices.description(),Lifetime::external,true));
    const auto i = take(graph.declare_buffer("indices",{12,vk::BufferUsageFlagBits::eTransferDst | vk::BufferUsageFlagBits::eIndexBuffer}));
    const auto output = take(graph.declare_buffer("readback",{4096,vk::BufferUsageFlagBits::eTransferDst,BufferMemory::readback}));
    const auto dead = take(graph.declare_buffer("unused",{16}));
    const auto color = take(graph.declare_image("color",{32,32,vk::Format::eR8G8B8A8Unorm,
        vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eColorAttachment}));
    add(graph,"upload",{access(upload_v,vk::PipelineStageFlagBits2::eCopy,vk::AccessFlagBits2::eTransferRead),
        access(v,vk::PipelineStageFlagBits2::eCopy,vk::AccessFlagBits2::eTransferWrite,true),
        access(upload_i,vk::PipelineStageFlagBits2::eCopy,vk::AccessFlagBits2::eTransferRead),
        access(i,vk::PipelineStageFlagBits2::eCopy,vk::AccessFlagBits2::eTransferWrite,true)});
    add(graph,"compute",{access(v,vk::PipelineStageFlagBits2::eComputeShader,vk::AccessFlagBits2::eShaderStorageRead | vk::AccessFlagBits2::eShaderStorageWrite)});
    add(graph,"clear",{access(color,vk::PipelineStageFlagBits2::eClear,vk::AccessFlagBits2::eTransferWrite,true,vk::ImageLayout::eTransferDstOptimal)});
    add(graph,"draw",{access(v,vk::PipelineStageFlagBits2::eVertexAttributeInput,vk::AccessFlagBits2::eVertexAttributeRead),
        access(i,vk::PipelineStageFlagBits2::eIndexInput,vk::AccessFlagBits2::eIndexRead),
        access(color,vk::PipelineStageFlagBits2::eColorAttachmentOutput,vk::AccessFlagBits2::eColorAttachmentRead | vk::AccessFlagBits2::eColorAttachmentWrite,false,vk::ImageLayout::eColorAttachmentOptimal)});
    add(graph,"readback",{access(color,vk::PipelineStageFlagBits2::eCopy,vk::AccessFlagBits2::eTransferRead,false,vk::ImageLayout::eTransferSrcOptimal),
        access(output,vk::PipelineStageFlagBits2::eCopy,vk::AccessFlagBits2::eTransferWrite,true)});
    add(graph,"culled",{access(dead,vk::PipelineStageFlagBits2::eClear,vk::AccessFlagBits2::eTransferWrite,true)});
    check(graph.mark_output(output)); const auto plan = take(graph.compile()); const auto report = take(format_plan(plan));
    require(plan.order().size() == 5,"combined graph culling failed");
    const auto filename = options.validation == ValidationMode::required ? "graph-pipeline-validation.txt" : "graph-pipeline.txt";
    std::ofstream log{filename,std::ios::trunc}; log << report.text();
    struct Context { ResourceFactory& factory; PipelineLayout& layout; ComputePipeline& compute; GraphicsPipeline& graphics; } context{factory,cl,cp,gp};
    const std::array callbacks{
        PassCallback{0,[](PassContext& pass,void*) -> Result<void> {
            if (auto status = pass.copy_buffer(0,1,48); !status) return status;
            return pass.copy_buffer(2,3,12);
        }},
        PassCallback{1,[](PassContext& pass,void* data) -> Result<void> {
            auto& c = *static_cast<Context*>(data); auto buffer = pass.buffer(1); if (!buffer) return std::unexpected(buffer.error());
            const std::array<BindingWrite,1> writes{{{0,0,BufferBinding{*buffer}}}};
            auto bindings = c.factory.create_bindings(c.layout,0,writes); if (!bindings) return std::unexpected(bindings.error());
            auto encoder = pass.compute(); if (!encoder) return std::unexpected(encoder.error());
            if (auto status = encoder->bind_pipeline(c.compute); !status) return status;
            const std::array sets{&*bindings}; if (auto status = encoder->bind_sets(sets); !status) return status;
            return encoder->dispatch({3,1,1});
        },&context},
        PassCallback{2,[](PassContext& pass,void*) {
            return pass.clear(6,vk::ClearColorValue{std::array<float,4>{0,0,0,1}},{vk::ImageAspectFlagBits::eColor,0,1,0,1});
        }},
        PassCallback{3,[](PassContext& pass,void* data) -> Result<void> {
            auto& c = *static_cast<Context*>(data);
            auto image = pass.image(6); if (!image) return std::unexpected(image.error());
            auto view = c.factory.create_view(**image); if (!view) return std::unexpected(view.error());
            auto vtx = pass.buffer(1); if (!vtx) return std::unexpected(vtx.error());
            auto idx = pass.buffer(3); if (!idx) return std::unexpected(idx.error());
            RenderingDesc desc{}; desc.color.view = &*view; desc.color.load = vk::AttachmentLoadOp::eLoad;
            auto render = pass.begin_rendering(desc); if (!render) return std::unexpected(render.error());
            if (auto status = render->bind_pipeline(c.graphics); !status) return status;
            if (auto status = render->vertex_buffer(0,**vtx); !status) return status;
            if (auto status = render->index_buffer(**idx,vk::IndexType::eUint32); !status) return status;
            const std::array<float,4> red{1,0,0,1};
            if (auto status = render->push_constants(vk::ShaderStageFlagBits::eFragment,0,std::as_bytes(std::span{red})); !status) return status;
            if (auto status = render->draw_indexed(3); !status) return status;
            return render->end();
        },&context},
        PassCallback{4,[](PassContext& pass,void*) { return pass.copy_to_buffer(6,4,{0,0,0,0,32,32}); }}};
    const std::array external{ExternalBinding{0,&vertices},ExternalBinding{2,&indices}};
    const std::array final{FinalAccess{4,{{vk::PipelineStageFlagBits2::eHost,vk::AccessFlagBits2::eHostRead}}}};
    for (unsigned repeat=0; repeat<4; ++repeat) {
        auto execution = take(execute(plan,queue,{external,callbacks,final,true}));
        require(execution.synchronization().size() == 12,"combined graph synchronization record count differs");
        bool compute_to_vertex=false;
        for (const auto& sync : execution.synchronization()) {
            if (sync.pass == 3 && sync.resource == 1) {
                compute_to_vertex = bool(sync.before.access & vk::AccessFlagBits2::eShaderStorageWrite) &&
                    sync.target.access == vk::AccessFlagBits2::eVertexAttributeRead;
            }
            if (repeat == 0) log << "barrier pass=" << (sync.pass ? std::to_string(*sync.pass) : "final") << " resource=" << sync.resource
                << " mip=" << sync.mip << " layer=" << sync.layer << " stages=" << static_cast<VkPipelineStageFlags2>(sync.before.stages)
                << "->" << static_cast<VkPipelineStageFlags2>(sync.target.stages) << " access=" << static_cast<VkAccessFlags2>(sync.before.access)
                << "->" << static_cast<VkAccessFlags2>(sync.target.access) << " layout=" << static_cast<int>(sync.before.layout)
                << "->" << static_cast<int>(sync.target.layout) << '\n';
        }
        require(compute_to_vertex,"compute-to-vertex synchronization missing");
        require(take(queue.wait(execution.submission())),"combined graph timed out");
        std::array<std::uint32_t,1024> pixels{}; check(take(execution.buffer(4))->read(0,std::as_writable_bytes(std::span{pixels})));
        require(pixels == reference,"combined graph differs from M5 indexed triangle baseline");
        execution = {};
        VmaTotalStatistics stats{}; vmaCalculateStatistics(queue.device().allocator(),&stats);
        require(stats.total.statistics.allocationCount == 2 && queue.stats().pending_slots == 0,"combined graph transient leak");
    }
    require(bool(log),"graph diagnostic artifact write failed"); check(queue.close());
}
