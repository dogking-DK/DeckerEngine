#include <dk/render/DiskScene.hpp>
#include <dk/render/ScenePipeline.hpp>
#include <dk/memory/MemorySystem.hpp>
#include <dk/memory/Context.hpp>
#include <dk/io/File.hpp>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <vector>
using namespace dk;
namespace {
template<class T, class E> T take(std::expected<T,E>&& result) {
    if (!result) {
        if constexpr (std::is_same_v<E,Error>) {
            auto text=result.error().message;
            for (const auto& c : result.error().context) text+=" / "+c;
            throw std::runtime_error(text);
        } else throw std::runtime_error("Memory operation failed");
    }
    return std::move(*result);
}
void check(Result<void> value) { if (!value) throw std::runtime_error(value.error().message); }
struct Diagnostics { std::atomic<unsigned> warnings=0,errors=0; };
void diagnostic(void* user,const graphics::Diagnostic& message) noexcept {
    auto& d=*static_cast<Diagnostics*>(user);
    if (message.severity==VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) ++d.errors;
    if (message.severity==VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) ++d.warnings;
    if (message.severity>=VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
        std::fprintf(stderr,"%.*s: %.*s\n",static_cast<int>(message.name.size()),message.name.data(),static_cast<int>(message.message.size()),message.message.data());
}
render::ViewDescription camera() {
    render::ViewDescription result{960,540,1};
    const Vec3d eye{8,1.8,0}, target{-4,2,0};
    const Vec3d forward=(target-eye).normalized(), right=forward.cross(Vec3d::UnitY()).normalized(), up=right.cross(forward);
    Mat4d world=Mat4d::Identity(); world.block<3,1>(0,0)=right; world.block<3,1>(0,1)=up;
    world.block<3,1>(0,2)=-forward; world.block<3,1>(0,3)=eye;
    result.camera_world=take(Transformd::from_matrix(world));
    constexpr double near=0.05,far=100;
    const double f=1/std::tan(65*3.14159265358979323846/360);
    result.projection=Mat4d::Zero(); result.projection(0,0)=f/(960.0/540); result.projection(1,1)=-f;
    result.projection(2,2)=far/(near-far); result.projection(2,3)=far*near/(near-far); result.projection(3,2)=-1;
    return result;
}
int run(const std::vector<std::filesystem::path>& args) {
    if (args.size()!=0 && args.size()!=3) {
        std::fprintf(stderr,"usage: dk-render-demo [PROJECT_ROOT MANIFEST OUTPUT.ppm]\n"); return 2;
    }
    const auto root=args.empty() ? std::filesystem::path{DK_DEMO_PROJECT_DIR} : args[0];
    const auto manifest=args.empty() ? std::filesystem::path{"project.json"} : args[1];
    const auto output=args.empty() ? std::filesystem::path{"render-sponza.ppm"} : args[2];
    if (args.empty() && !std::filesystem::exists(root/"assets/gltf/Sponza/glTF/Sponza.gltf")) {
        std::fprintf(stderr,"SKIP: local Sponza payload missing; see projects/demo/assets/README.md\n"); return 77;
    }
    if (output.extension()!=".ppm") { std::fprintf(stderr,"output must have .ppm extension\n"); return 2; }
    auto system=take(memory::MemorySystem::create()); auto heap=take(system.create_heap({"disk-render-demo",memory::DomainCategory::render}));
    Diagnostics diagnostics;
    {
        memory::ThreadContext context{system,heap}; memory::ExecutionScope scope{context,heap};
        auto project=take(Project::open(root,manifest));
        auto source=take(render::DiskScene::load(heap,project,{GltfImportProfile::unlit_preview}));
        std::size_t primitive_count=0,texture_count=0;
        for (const auto& asset : source.assets()) {
            primitive_count+=asset->mesh.primitives.size(); texture_count+=asset->textures.size();
            for (const auto& text : asset->diagnostics) std::fprintf(stderr,"import: %s\n",text.c_str());
        }
        graphics::DeviceOptions options; options.validation=graphics::ValidationMode::required;
        options.diagnostic_sink=diagnostic; options.diagnostic_user_data=&diagnostics;
        auto device=graphics::Device::create(heap,options);
        if (!device) {
            std::fprintf(stderr,"%s\n",device.error().message.c_str());
            return device.error().code==ErrorCode::not_found || device.error().code==ErrorCode::not_supported ? 77 : 1;
        }
        auto queue=take(graphics::SubmissionQueue::create(heap,std::move(*device)));
        std::printf("GPU=%s driver=%s\n",queue.device().adapter().properties.deviceName.data(),queue.device().adapter().driver.driverInfo.data());
        auto assets=take(source.upload(queue)); auto pipeline=take(render::ScenePipeline::create(heap,queue,DK_RENDER_SHADER_DIR));
        auto view=take(render::RenderView::create(source.scene(),camera()));
        auto frame=take(pipeline.render(queue,view,assets,{{0.04f,0.08f,0.16f},1,true}));
        if (!take(frame.wait(queue))) throw std::runtime_error("frame timed out");
        std::vector<std::byte> rgba(static_cast<std::size_t>(frame.info().width)*frame.info().height*4);
        check(frame.read_rgba8(rgba));
        const std::string header="P6\n"+std::to_string(frame.info().width)+" "+std::to_string(frame.info().height)+"\n255\n";
        const auto header_bytes=std::as_bytes(std::span{header.data(),header.size()});
        ByteBuffer ppm{header_bytes.begin(),header_bytes.end()}; ppm.reserve(header.size()+rgba.size()/4*3);
        unsigned low=255,high=0;
        std::size_t varied=0;
        for (std::size_t i=0;i<rgba.size();i+=4)
            if (rgba[i]!=rgba[0] || rgba[i+1]!=rgba[1] || rgba[i+2]!=rgba[2]) ++varied;
        for (std::size_t i=0;i<rgba.size();i+=4) for (std::size_t c=0;c<3;++c) {
            const auto v=std::to_integer<unsigned>(rgba[i+c]); low=std::min(low,v); high=std::max(high,v); ppm.push_back(rgba[i+c]);
        }
        if (high-low<20 || varied<rgba.size()/4/20 || !frame.info().draw_count) throw std::runtime_error("scene image is empty or nearly uniform");
        check(write_file_bytes_atomic(output,ppm));
        auto plan=output; plan.replace_extension("txt");
        check(write_file_bytes_atomic(plan,std::as_bytes(std::span{frame.plan_text().data(),frame.plan_text().size()})));
        std::printf("scene=%s revision=%llu frame=%llu draws=%zu primitives=%zu textures=%zu extent=%ux%u output=%s\n",
            frame.info().scene.to_string().c_str(),static_cast<unsigned long long>(frame.info().revision),static_cast<unsigned long long>(frame.info().frame),
            frame.info().draw_count,primitive_count,texture_count,frame.info().width,frame.info().height,take(path_to_utf8(output)).c_str());
        check(queue.close());
    }
    const auto live=heap.snapshot().live_allocations;
    std::printf("errors=%u warnings=%u liveAllocations=%zu\n",diagnostics.errors.load(),diagnostics.warnings.load(),live);
    return diagnostics.errors==0 && diagnostics.warnings==0 && live==0 && system.try_close().closed() ? 0 : 1;
}
}
#ifdef _WIN32
int wmain(int argc,wchar_t** argv) {
#else
int main(int argc,char** argv) {
#endif
    try {
        std::vector<std::filesystem::path> args; for (int i=1;i<argc;++i) args.emplace_back(argv[i]);
        return run(args);
    } catch (const std::exception& error) { std::fprintf(stderr,"disk render: %s\n",error.what()); return 1; }
}
