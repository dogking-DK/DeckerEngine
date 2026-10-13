#include <dk/render/ScenePipeline.hpp>
#include <dk/render/Atmosphere.hpp>
#include <dk/scene/SceneDocument.hpp>
#include <dk/memory/MemorySystem.hpp>
#include <dk/memory/Context.hpp>
#include <dk/io/File.hpp>
#include <atomic>
#include <cstdio>
#include <numbers>
#include <stdexcept>
#include <vector>
using namespace dk;
namespace {
template<class T,class E> T take(std::expected<T,E> r) {
    if(!r) { if constexpr(std::is_same_v<E,Error>) throw std::runtime_error(r.error().message); else throw std::bad_alloc{}; }
    return std::move(*r);
}
void check(Result<void> r) { if(!r) throw std::runtime_error(r.error().message); }
struct Diagnostics { std::atomic<unsigned> errors=0,warnings=0; };
void diagnostic(void* pointer,const graphics::Diagnostic& message) noexcept {
    auto& d=*static_cast<Diagnostics*>(pointer);
    if(message.severity==VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) ++d.errors;
    if(message.severity==VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) ++d.warnings;
    if(message.severity>=VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
        std::fprintf(stderr,"%.*s\n",int(message.message.size()),message.message.data());
}
CpuAsset sphere() {
    CpuAsset asset; asset.mesh.id=take(AssetId::generate());
    MaterialData m; m.id=take(AssetId::generate()); m.base_color={.8f,.8f,.8f,1};
    asset.materials.push_back(m); MeshPrimitive mesh; mesh.material=m.id;
    for(unsigned y=0;y<=32;++y) for(unsigned x=0;x<=64;++x) {
        const double theta=std::numbers::pi*y/32,phi=2*std::numbers::pi*x/64;
        const Vec3f n{float(std::sin(theta)*std::cos(phi)),float(std::cos(theta)),float(std::sin(theta)*std::sin(phi))};
        mesh.positions.push_back(n*.8f); mesh.normals.push_back(n);
    }
    for(unsigned y=0;y<32;++y) for(unsigned x=0;x<64;++x) {
        const auto a=y*65+x,b=a+65;
        for(auto i:{a,a+1,b,b,a+1,b+1}) mesh.indices.push_back(i);
    }
    asset.mesh.primitives.push_back(std::move(mesh)); return asset;
}
int run(const std::filesystem::path& directory) {
    std::filesystem::create_directories(directory);
    auto system=take(memory::MemorySystem::create()); auto heap=take(system.create_heap({"sunlight-demo",memory::DomainCategory::render}));
    Diagnostics diagnostics;
    {
        memory::ThreadContext context{system}; memory::ExecutionScope scope{context,heap};
        graphics::DeviceOptions options; options.validation=graphics::ValidationMode::required;
        options.diagnostic_sink=diagnostic; options.diagnostic_user_data=&diagnostics;
        auto queue=take(graphics::SubmissionQueue::create(heap,take(graphics::Device::create(heap,options))));
        auto atmosphere=take(render::AtmosphereRenderer::create(heap,queue,DK_ATMOSPHERE_SHADER_DIR));
        auto pipeline=take(render::ScenePipeline::create(heap,queue,DK_RENDER_SHADER_DIR));
        auto assets=take(render::GpuAssets::create(heap)); auto asset=sphere();
        auto uploaded=take(assets.upload(queue,asset)); if(!take(uploaded.wait(queue))) throw std::runtime_error("upload incomplete");
        auto scene=take(SceneDocument::create());
        for(int i=0;i<3;++i) {
            const auto id=take(EntityId::generate()); check(scene->create_entity(id));
            Trsd trs; trs.translation={2.1*(i-1),2,0};
            if(i==1) trs.scale={1,.65,1};
            if(i==2) trs.scale={-.8,1,.8};
            check(scene->set_local_transform(id,trs));
            check(scene->set_asset_references(id,{{asset.mesh.id,AssetKind::mesh}}));
        }
        render::ViewDescription desc{960,360}; Trsd camera; camera.translation={0,2,3};
        desc.camera_world=take(Transformd::from_trs(camera)); desc.projection(0,0)=1/3.3; desc.projection(1,1)=-1/1.2375; desc.projection(2,2)=-.15;
        EnvironmentDescription e; e.enabled=true;
        struct Preset { const char* name; double sun_y; };
        for(const auto preset:{Preset{"noon",.7},Preset{"sunset",.025},Preset{"twilight",-.06}}) {
            e.sun.direction={-.6,preset.sun_y,std::sqrt(1-.36-preset.sun_y*preset.sun_y)};
            check(scene->set_environment(e)); auto sky=take(atmosphere.prepare(queue,scene->environment(),Vec3d{0,2,0}));
            auto view=take(render::RenderView::create(take(render::RenderScene::extract(heap,take(scene->snapshot()))),desc));
            render::RenderSettings settings{{.012f,.018f,.028f},5,false};
            // Three complete images make the effect of each optional path directly comparable.
            for(const auto mode:{std::string_view{"unlit"},std::string_view{"direct"},std::string_view{"sky"}}) {
                settings.sky_diffuse=mode=="sky";
                auto frame=take(mode=="unlit" ? pipeline.render(queue,view,assets,settings) : pipeline.render_sunlight(queue,view,assets,sky,settings));
                if(!take(frame.wait(queue))) throw std::runtime_error("frame incomplete");
                std::vector<std::byte> rgba(std::size_t(desc.width)*desc.height*4); check(frame.read_rgba8(rgba));
                const auto header="P6\n"+std::to_string(desc.width)+" "+std::to_string(desc.height)+"\n255\n";
                const auto bytes=std::as_bytes(std::span{header.data(),header.size()}); ByteBuffer ppm{bytes.begin(),bytes.end()};
                for(std::size_t i=0;i<rgba.size();i+=4) ppm.insert(ppm.end(),rgba.begin()+i,rgba.begin()+i+3);
                const auto name=std::string(preset.name)+"-"+std::string(mode)+".ppm"; check(write_file_bytes_atomic(directory/name,ppm));
                std::printf("%s exposure=5 frame_revision=%llu\n",name.c_str(),static_cast<unsigned long long>(frame.info().revision));
            }
        }
        check(queue.close());
    }
    std::printf("validation errors=%u warnings=%u live=%zu\n",diagnostics.errors.load(),diagnostics.warnings.load(),heap.snapshot().live_allocations);
    return !diagnostics.errors && !diagnostics.warnings && !heap.snapshot().live_allocations && system.try_close().closed() ? 0 : 1;
}
}
#ifdef _WIN32
int wmain(int argc,wchar_t** argv) {
#else
int main(int argc,char** argv) {
#endif
    if(argc>2) { std::fprintf(stderr,"usage: dk-sunlight-demo [OUTPUT_DIRECTORY]\n"); return 2; }
    try { return run(argc==2 ? std::filesystem::path{argv[1]} : std::filesystem::path{"sunlight-output"}); }
    catch(const std::exception& error) { std::fprintf(stderr,"sunlight demo: %s\n",error.what()); return 1; }
}
