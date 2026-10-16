#pragma once
#include <dk/render/PathTraceRenderer.hpp>
#include <dk/render/DiskScene.hpp>
#include <dk/scene/SceneDocument.hpp>
#include <dk/memory/MemorySystem.hpp>
#include <dk/memory/Context.hpp>
#include <dk/graphics/CommandEncoder.hpp>
#include <dk/graphics/Transfer.hpp>
#include <dk/profiling/Profiler.hpp>
#include <nlohmann/json.hpp>
#include <fstream>
#include <chrono>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <algorithm>

using namespace dk;
using namespace dk::render;
using namespace dk::graphics;
using Json=nlohmann::json;
using Clock=std::chrono::steady_clock;
namespace {
void require(bool v,const char* message) { if(!v) throw std::runtime_error(message); }
template<class T,class E> T take(std::expected<T,E> r) {
    if(!r) { if constexpr(std::is_same_v<E,Error>) throw std::runtime_error(r.error().message); else throw std::bad_alloc{}; }
    return std::move(*r);
}
void check(Result<void> r) { if(!r) throw std::runtime_error(r.error().message); }
double elapsed(Clock::time_point t) { return std::chrono::duration<double,std::milli>(Clock::now()-t).count(); }
struct Diagnostics { std::atomic<unsigned> errors=0,warnings=0; };
void diagnostic(void* user,const Diagnostic& d) noexcept {
    auto& c=*static_cast<Diagnostics*>(user);
    if(d.severity==VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) ++c.errors;
    if(d.severity==VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) ++c.warnings;
    if(d.severity>=VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) std::fprintf(stderr,"%.*s\n",int(d.message.size()),d.message.data());
}
Json pixels(std::span<const Vec3d> data) {
    Json values=Json::array(); for(const auto& v:data) { require(v.allFinite(),"Nonfinite linear pixel"); values.push_back({v.x(),v.y(),v.z()}); } return values;
}
Json profile(Submission ticket,bool enabled) {
    if(!ticket.value()) return nullptr;
    const auto p=ticket.gpu_profile();
    if(!enabled) { require(p.status()==GpuProfileStatus::disabled,"Unexpected timestamp acquisition"); return nullptr; }
    require(p.status()==GpuProfileStatus::ready && !p.dropped_zones() && p.timestamp_valid_bits()>0 && p.timestamp_period_ns()>0,"Missing GPU timestamps");
    Json out={{"submission",ticket.value()},{"period_ns",p.timestamp_period_ns()},{"valid_bits",p.timestamp_valid_bits()},{"zones",Json::array()}};
    unsigned count=0;
    for(const auto& t:p.timings()) {
        require(std::isfinite(t.elapsed_ns) && t.elapsed_ns>=0,"Invalid GPU timing");
        out["zones"].push_back({{"name",t.name},{"gpu_ms",t.elapsed_ns/1e6},{"cpu_record_ms",double(t.cpu_end_ns-t.cpu_begin_ns)/1e6}});
        if(t.kind==GpuZoneKind::submission) { out["submission_ms"]=t.elapsed_ns/1e6; ++count; }
    }
    require(count==1,"Missing total GPU interval"); return out;
}
Json memory(const PathTraceRenderer& r,const SubmissionQueue& q) {
    const auto s=r.memory_stats(q);
    return {{"limit",s.owned.limit},{"current",s.owned.current},{"peak",s.owned.peak},
        {"device_local",s.owned.device_local},{"host_visible",s.owned.host_visible},
        {"vma_allocation_bytes",s.device_allocation_bytes},{"vma_block_bytes",s.device_block_bytes}};
}
struct Fixture {
    memory::ResourceHandle heap;
    std::unique_ptr<SceneDocument> document=take(SceneDocument::create(take(SceneId::parse("30000000-0000-4000-8000-000000000011"))));
    CpuAsset asset;
    DiskScene disk;
    RenderScene source;
    PathTraceScene cpu;
    PathTraceMaterialModel model=PathTraceMaterialModel::lambert;
    Vec3d eye{0,0,1};
    explicit Fixture(memory::ResourceHandle h,std::string_view name):heap(h) {
        if(name=="disk-mask") {
            auto project=take(Project::open(DK_PATH_DISK_DIR,"project.json"));
            DiskSceneOptions options; options.profile=GltfImportProfile::unlit_preview;
            disk=take(DiskScene::load(heap,project,options)); source=disk.scene();
            std::vector<const CpuAsset*> assets; for(const auto& a:disk.assets()) assets.push_back(a.get());
            model=PathTraceMaterialModel::metallic_roughness;
            cpu=take(PathTraceScene::create(heap,source,assets,{},model)); eye={0,0,4}; return;
        }
        asset.mesh.id=take(AssetId::parse("10000000-0000-4000-8000-000000000011"));
        const auto entity=take(EntityId::parse("20000000-0000-4000-8000-000000000011"));
        check(document->create_entity(entity)); check(document->set_asset_references(entity,{{asset.mesh.id,AssetKind::mesh}}));
        quad({-2,-2,0},{2,-2,0},{2,2,0},{-2,2,0},{.5f,.4f,.3f});
        quad({-.5f,-.5f,2},{-.5f,.5f,2},{.5f,.5f,2},{.5f,-.5f,2},{0,0,0},{3,3,3});
        if(name=="ggx-box") {
            model=PathTraceMaterialModel::metallic_roughness;
            quad({-2,-2,0},{-2,2,0},{-2,2,2},{-2,-2,2},{.5f,.1f,.1f});
            quad({2,2,0},{2,-2,0},{2,-2,2},{2,2,2},{.1f,.5f,.1f});
            quad({-2,2,0},{2,2,0},{2,2,2},{-2,2,2},{.4f,.4f,.4f});
            quad({2,-2,0},{-2,-2,0},{-2,-2,2},{2,-2,2},{.4f,.4f,.4f});
            asset.materials[0].metallic=.5f; asset.materials[0].roughness=.5f;
        } else require(name=="area-lambert","Unknown fixture");
        source=take(RenderScene::extract(heap,take(document->snapshot())));
        const std::array assets{&asset}; cpu=take(PathTraceScene::create(heap,source,assets,{},model));
    }
    void quad(Vec3f a,Vec3f b,Vec3f c,Vec3f d,Vec3f color,Vec3f emission={0,0,0}) {
        MaterialData material; material.id=take(AssetId::generate()); material.metallic=0; material.roughness=1;
        material.base_color={color.x(),color.y(),color.z(),1}; material.emissive=emission; material.double_sided=true;
        asset.materials.push_back(material);
        MeshPrimitive primitive; primitive.positions={a,b,c,d}; primitive.indices={0,1,2,0,2,3}; primitive.material=material.id;
        asset.mesh.primitives.push_back(std::move(primitive));
    }
    RenderView view(std::uint32_t width=4,std::uint32_t height=4,double offset=0) const {
        ViewDescription d{width,height}; Trsd camera; camera.translation=eye+Vec3d{offset,0,0}; d.camera_world=take(Transformd::from_trs(camera));
        d.projection=Mat4d::Zero(); d.projection(0,0)=2.*height/width; d.projection(1,1)=-2;
        d.projection(2,2)=100./(.05-100); d.projection(2,3)=100.*.05/(.05-100); d.projection(3,2)=-1;
        return take(RenderView::create(source,d));
    }
    PathTraceSettings settings() const {
        PathTraceSettings s; s.max_bounces=4; s.rr_start=3; s.environment_rgb={.15,.2,.3}; return s;
    }
    Json metadata(std::string_view name) const {
        const auto v=view().description(); Json projection=Json::array(); for(int row=0;row<4;++row) for(int col=0;col<4;++col) projection.push_back(v.projection(row,col));
        return {{"name",name},{"fingerprint",cpu.fingerprint()},{"fingerprint_version",3},{"scene",source.id().to_string()},
            {"revision",source.revision()},{"triangles",cpu.triangle_count()},{"textures",cpu.texture_count()},
            {"material_model",model==PathTraceMaterialModel::lambert ? "lambert" : "metallic_roughness"},
            {"eye",{eye.x(),eye.y(),eye.z()}},{"quality_projection",projection},{"quality_width",4},{"quality_height",4},
            {"settings",{{"model",3},{"sampler",1},{"max_bounces",4},{"rr_start",3},{"environment",{.15,.2,.3}},{"sun","none"},{"estimator","mis"}}}};
    }
};
PathTraceGpuImage read_image(SubmissionQueue& q,const PathTraceFrame& f) {
    require(take(q.wait(f.submission())),"GPU wait failed"); auto result=take(f.read(q)); require(bool(result),"Readback not completed"); return std::move(*result);
}
// An exclusive benchmark worker can explicitly wait; product owner threads still use poll.
Json advance(PathTraceRenderer& r,SubmissionQueue& q,std::uint64_t id,bool timing) {
    Json tiles=Json::array(); const auto start=Clock::now();
    while(take(r.status(id)).completed_spp<take(r.status(id)).target_spp) {
        const auto before=Clock::now(); require(take(r.submit_batch(q,id)),"Tile did not submit"); const double record_ms=elapsed(before);
        const auto ticket=r.pending_submission(); require(ticket.value()!=0,"Missing pending tile ticket");
        const auto wait=Clock::now(); require(take(q.wait(ticket)),"Tile wait failed"); (void)take(r.poll(q));
        if(timing) tiles.push_back({{"record_submit_ms",record_ms},{"wait_poll_ms",elapsed(wait)},{"gpu",profile(ticket,true)}});
    }
    return {{"wall_ms",elapsed(start)},{"completed_spp",take(r.status(id)).completed_spp},{"tiles",std::move(tiles)}};
}
}
