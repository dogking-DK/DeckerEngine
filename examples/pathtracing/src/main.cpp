#include <dk/render/PathTraceReference.hpp>
#include <dk/scene/SceneDocument.hpp>
#include <dk/memory/MemorySystem.hpp>
#include <dk/memory/Context.hpp>
#include <dk/io/File.hpp>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <format>
#include <stdexcept>

using namespace dk;
using namespace dk::render;
namespace {
template<class T> T take(Result<T> value) { if (!value) throw std::runtime_error(value.error().message); return std::move(*value); }
void check(Result<void> value) { if (!value) throw std::runtime_error(value.error().message); }
void quad(CpuAsset& asset,Vec3f a,Vec3f b,Vec3f c,Vec3f d,Vec3f color,Vec3f emission={0,0,0}) {
    MaterialData m; m.id=take(AssetId::parse(std::format("00000000-0000-4000-8000-{:012}",asset.materials.size()+1)));
    m.metallic=0; m.base_color={color.x(),color.y(),color.z(),1}; m.emissive=emission; asset.materials.push_back(m);
    MeshPrimitive p; p.material=m.id; p.positions={a,b,c,d}; p.indices={0,1,2,0,2,3}; asset.mesh.primitives.push_back(std::move(p));
}
int run(const std::filesystem::path& directory,std::uint32_t spp) {
    if (!spp || spp>4096) throw std::runtime_error("spp must be 1..4096");
    std::filesystem::create_directories(directory);
    auto system=memory::MemorySystem::create().value(); auto heap=system.create_heap({"path-reference-demo",memory::DomainCategory::render}).value();
    {
        memory::ThreadContext context{system}; memory::ExecutionScope scope{context,heap};
        CpuAsset asset; asset.mesh.id=take(AssetId::parse("00000000-0000-4000-8000-000000000010"));
        const Vec3f white{.65f,.65f,.65f};
        quad(asset,{-1,0,-1},{1,0,-1},{1,2,-1},{-1,2,-1},white);
        quad(asset,{-1,0,1},{1,0,1},{1,0,-1},{-1,0,-1},white);
        quad(asset,{-1,2,-1},{1,2,-1},{1,2,1},{-1,2,1},white);
        quad(asset,{-1,0,-1},{-1,2,-1},{-1,2,1},{-1,0,1},{.7f,.12f,.08f});
        quad(asset,{1,0,1},{1,2,1},{1,2,-1},{1,0,-1},{.12f,.6f,.15f});
        quad(asset,{-.5f,1.99f,-.4f},{.5f,1.99f,-.4f},{.5f,1.99f,.4f},{-.5f,1.99f,.4f},{0,0,0},{5,5,5});
        auto document=take(SceneDocument::create(take(SceneId::parse("00000000-0000-4000-8000-000000000020"))));
        const auto entity=take(EntityId::parse("00000000-0000-4000-8000-000000000030")); check(document->create_entity(entity));
        check(document->set_asset_references(entity,{{asset.mesh.id,AssetKind::mesh}}));
        const auto source=take(RenderScene::extract(heap,take(document->snapshot())));
        const std::array inputs{static_cast<const CpuAsset*>(&asset)}; const auto scene=take(PathTraceScene::create(heap,source,inputs));
        ViewDescription d{24,24}; Trsd camera; camera.translation={0,1,3}; d.camera_world=take(Transformd::from_trs(camera));
        d.projection.setZero(); d.projection(0,0)=2.414213562373095; d.projection(1,1)=-d.projection(0,0);
        d.projection(2,2)=10.0/(.1-10); d.projection(2,3)=1.0/(.1-10); d.projection(3,2)=-1;
        auto view=take(RenderView::create(source,d)); PathTraceSettings settings; settings.seed=42; settings.max_bounces=4; settings.rr_start=3;
        const auto start=std::chrono::steady_clock::now();
        const auto image=take(render_path_reference(heap,scene,view,settings,{0,spp}));
        const double seconds=std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();
        const std::string header="P6\n24 24\n255\n"; const auto head=std::as_bytes(std::span{header.data(),header.size()});
        ByteBuffer bytes(head.begin(),head.end()); nlohmann::json pixels=nlohmann::json::array();
        double maximum=0;
        for (const auto& pixel:image.linear_rgb()) {
            pixels.push_back({pixel.x(),pixel.y(),pixel.z()}); maximum=std::max(maximum,pixel.maxCoeff());
            for (int k=0;k<3;++k) {
                const double c=1-1/(1+pixel[k]),srgb=c<=.0031308 ? 12.92*c : 1.055*std::pow(c,1/2.4)-.055;
                bytes.push_back(static_cast<std::byte>(std::clamp(std::lround(srgb*255),0l,255l)));
            }
        }
        if (!std::isfinite(maximum) || maximum<=0) throw std::runtime_error("Reference image is empty or nonfinite");
        nlohmann::json report{{"schema_version",1},{"renderer","cpu_lambert_reference_v2"},{"model_version",settings.model_version},{"sampler_version",1},
            {"scene",scene.id().to_string()},{"revision",scene.revision()},{"fingerprint",scene.fingerprint()},
            {"width",24},{"height",24},{"seed",42},{"first_sample",0},{"spp",spp},{"max_bounces",4},{"rr_start",3},
            {"estimator","mis"},{"environment_rgb",{0,0,0}},{"sun","none"},{"surface_only",false},
            {"exposure",1},{"tone","reinhard_srgb"},{"triangles",scene.triangle_count()},
            {"rays",image.info().statistics.rays},{"triangle_tests",image.info().statistics.triangle_tests},
            {"cpu_seconds",seconds},{"gpu_ms",nullptr},{"linear_rgb",std::move(pixels)}};
        auto matrix=[](const Mat4d& m) { nlohmann::json result=nlohmann::json::array(); for(int r=0;r<4;++r) for(int c=0;c<4;++c) result.push_back(m(r,c)); return result; };
        report["camera_world_row_major"]=matrix(d.camera_world.matrix()); report["projection_row_major"]=matrix(d.projection);
        report["max_rays"]=settings.max_rays; report["max_triangle_tests"]=settings.max_triangle_tests;
        const auto json=report.dump(2);
        check(write_file_bytes_atomic(directory/"reference.ppm",bytes));
        check(write_file_bytes_atomic(directory/"reference.json",std::as_bytes(std::span{json.data(),json.size()})));
        std::printf("CPU reference: 24x24, spp=%u, triangles=%zu, seconds=%.3f\n",spp,scene.triangle_count(),seconds);
    }
    if (heap.snapshot().live_allocations) throw std::runtime_error("Reference example leaked heap allocations");
    heap.begin_close(); return system.try_close().closed() ? 0 : 1;
}
}
#ifdef _WIN32
int wmain(int argc,wchar_t** argv) {
#else
int main(int argc,char** argv) {
#endif
    if (argc<2 || argc>3) { std::fprintf(stderr,"usage: dk-pathtracing-reference-demo OUTPUT_DIRECTORY [SPP=128]\n"); return 2; }
    try {
        std::uint32_t spp=128;
        if (argc==3) {
            const auto value=std::filesystem::path{argv[2]}.string(); std::size_t end=0; const auto number=std::stoul(value,&end);
            if (end!=value.size() || number>4096) throw std::runtime_error("Invalid spp"); spp=static_cast<std::uint32_t>(number);
        }
        return run(std::filesystem::path{argv[1]},spp);
    } catch (const std::exception& e) { std::fprintf(stderr,"path reference: %s\n",e.what()); return 1; }
}
