#include "../RenderAssetSupport.hpp"
#include <dk/render/PathTraceRenderer.hpp>
#include <dk/math/Projection.hpp>
#include <dk/scene/SceneDocument.hpp>
#include <dk/graphics/ShaderCompiler.hpp>
#include <dk/graphics/GpuObjects.hpp>
#include <dk/graphics/Transfer.hpp>
#include <dk/graphics/GraphExecution.hpp>
#ifdef DK_PATH_ASSET_DIR
#include <dk/assets/AssetCompiler.hpp>
#include <dk/assets/CpuArtifact.hpp>
#endif
#include "SubmissionInternal.hpp"
#include <atomic>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <numbers>
#include <stdexcept>

using namespace dk;
using namespace dk::render;
using namespace dk::graphics;
namespace {
void require(bool value,const char* message) { if (!value) throw std::runtime_error(message); }
template<class T> T take(Result<T> r) { if (!r) { for (const auto& c:r.error().context) std::fprintf(stderr,"%s\n",c.c_str()); throw std::runtime_error(r.error().message); } return std::move(*r); }
void check(Result<void> r) { if (!r) throw std::runtime_error(r.error().message); }
struct Diagnostics { std::atomic<unsigned> errors=0,warnings=0; };
void diagnostic(void* p,const Diagnostic& d) noexcept {
    auto& c=*static_cast<Diagnostics*>(p);
    if (d.severity==VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) ++c.errors;
    if (d.severity==VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) ++c.warnings;
    if (d.severity>=VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) std::fprintf(stderr,"%.*s: %.*s\n",int(d.name.size()),d.name.data(),int(d.message.size()),d.message.data());
}
PFN_vkQueueSubmit2 native_submit=nullptr; PFN_vkWaitSemaphores native_wait=nullptr;
bool fail_submit=false,timeout_once=false,lose_wait=false;
VKAPI_ATTR VkResult VKAPI_CALL submit_override(VkQueue q,std::uint32_t n,const VkSubmitInfo2* s,VkFence f) {
    if (std::exchange(fail_submit,false)) return VK_ERROR_OUT_OF_HOST_MEMORY; return native_submit(q,n,s,f);
}
VKAPI_ATTR VkResult VKAPI_CALL wait_override(VkDevice d,const VkSemaphoreWaitInfo* i,std::uint64_t t) {
    if (std::exchange(lose_wait,false)) return VK_ERROR_DEVICE_LOST;
    if (std::exchange(timeout_once,false)) return VK_TIMEOUT; return native_wait(d,i,t);
}
struct Fixture {
    memory::ResourceHandle heap;
    std::unique_ptr<SceneDocument> document=SceneDocument::create().value(); CpuAsset asset; EntityId entity;
    explicit Fixture(memory::ResourceHandle h):heap(h) {
        asset.mesh.id=AssetId::generate().value(); entity=document->create_entity().value();
        document->set_asset_references(entity,{{asset.mesh.id,AssetKind::mesh}}).value();
    }
    void triangle(Vec3f a,Vec3f b,Vec3f c,Vec3f color,Vec3f emission={0,0,0},bool two=false) {
        MaterialData m; m.id=AssetId::generate().value(); m.metallic=0; m.base_color={color.x(),color.y(),color.z(),1}; m.emissive=emission; m.double_sided=two;
        asset.materials.push_back(m); MeshPrimitive p; p.positions={a,b,c}; p.indices={0,1,2}; p.material=m.id; asset.mesh.primitives.push_back(std::move(p));
    }
    void quad(float lo,float hi,float z,Vec3f color,Vec3f emission={0,0,0},bool flip=false) {
        const Vec3f a{lo,lo,z},b{hi,lo,z},c{hi,hi,z},d{lo,hi,z};
        if (flip) { triangle(a,c,b,color,emission); triangle(a,d,c,color,emission); }
        else { triangle(a,b,c,color,emission); triangle(a,c,d,color,emission); }
    }
    RenderScene source() const { return RenderScene::extract(heap,document->snapshot().value()).value(); }
    PathTraceScene scene() const { const std::array assets{&asset}; return take(PathTraceScene::create(heap,source(),assets)); }
    RenderView view(const RenderScene& source,std::uint32_t size=4) const {
        ViewDescription d{size,size}; Trsd camera; camera.translation={0,0,1}; d.camera_world=Transformd::from_trs(camera).value();
        d.projection.setZero(); d.projection(0,0)=2; d.projection(1,1)=-2;
        const double near=.1,far=10; d.projection(2,2)=far/(near-far); d.projection(2,3)=far*near/(near-far); d.projection(3,2)=-1;
        return take(RenderView::create(source,d));
    }
};
const PathTraceRay down{{0,0,1},{0,0,-1}};
PathTraceSettings settings(std::uint32_t depth=1) { PathTraceSettings s; s.max_bounces=depth; s.rr_start=0; return s; }
PathTraceGpuImage complete(SubmissionQueue& q,const PathTraceFrame& frame) {
    require(take(q.wait(frame.submission())),"GPU frame wait failed"); auto result=take(frame.read(q)); require(bool(result),"completed frame still pending"); return std::move(*result);
}
PathTraceGpuImage ray_image(PathTraceRenderer& renderer,SubmissionQueue& queue,const PathTraceGpuScene& scene,PathTraceSettings s,
    std::uint32_t samples,const PathTraceRay& ray=down,std::uint32_t first=0) {
    return complete(queue,take(renderer.submit_rays(queue,scene,std::span{&ray,1},s,{first,samples})));
}
void close_color(const Vec3d& value,const Vec3d& expected,double tolerance=1e-4) {
    if ((value-expected).cwiseAbs().maxCoeff()>tolerance) { std::printf("actual %.9g %.9g %.9g expected %.9g %.9g %.9g\n",value.x(),value.y(),value.z(),expected.x(),expected.y(),expected.z()); throw std::runtime_error("linear color mismatch"); }
}
void analytic(memory::ResourceHandle heap,PathTraceRenderer& renderer,SubmissionQueue& queue) {
    Fixture f{heap}; auto s=settings(); auto empty=take(renderer.prepare_scene(queue,f.scene()));
    require(empty.submission().value()==0,"empty scene built an AS"); auto black=ray_image(renderer,queue,empty,s,1); close_color(black.linear_rgb()[0],Vec3d::Zero());
    s.environment_rgb={.25,.5,.75}; auto sky=ray_image(renderer,queue,empty,s,32); close_color(sky.linear_rgb()[0],s.environment_rgb);
    close_color(sky.second_moment()[0],s.environment_rgb.cwiseProduct(s.environment_rgb)); require(!sky.primary_hits()[0],"empty scene hit");
    f.quad(-10,10,0,{.5f,.25f,1}); auto source=f.scene(); auto scene=take(renderer.prepare_scene(queue,source)); s.estimator=PathTraceEstimator::bsdf_only;
    close_color(ray_image(renderer,queue,scene,s,256).linear_rgb()[0],{.125,.125,.75});
    for (auto& m:f.asset.materials) { m.emissive={2,3,4}; m.base_color={0,0,0,1}; }
    auto emissive=take(renderer.prepare_scene(queue,f.scene())); s.environment_rgb.setZero(); close_color(ray_image(renderer,queue,emissive,s,16).linear_rgb()[0],{2,3,4});
    for (auto& m:f.asset.materials) { m.emissive.setZero(); m.base_color={.5f,.25f,1,1}; }
    EnvironmentDescription env; env.sun.direction={std::sqrt(.5),0,std::sqrt(.5)}; env.sun.irradiance={2,3,4}; f.document->set_environment(env).value();
    auto sun=take(renderer.prepare_scene(queue,f.scene())); s.sun=PathTraceSun::scene;
    const Vec3d expected=Vec3d{.5,.25,1}.cwiseProduct(Vec3d{2,3,4})*(std::sqrt(.5)/std::numbers::pi);
    close_color(ray_image(renderer,queue,sun,s,32).linear_rgb()[0],expected);
    f.triangle({.2f,-2,.5f},{2,2,.5f},{2,-2,.5f},{0,0,0},{0,0,0},true);
    f.triangle({.2f,-2,.5f},{.2f,2,.5f},{2,2,.5f},{0,0,0},{0,0,0},true);
    auto occluded=take(renderer.prepare_scene(queue,f.scene())); const PathTraceRay clipped{{0,0,.1},{0,0,-1},0,.2};
    close_color(ray_image(renderer,queue,occluded,s,16,clipped).linear_rgb()[0],Vec3d::Zero());
    for (const double scale:{1e-6,1.0,1e4}) {
        Fixture t{heap}; t.triangle({-1,-1,0},{1,-1,0},{0,1,0},{.3f,.4f,.5f},{0,0,0},true);
        const auto parent=t.document->create_entity().value(); Trsd p; p.scale={-2*scale,3*scale,.5*scale}; p.translation={2*scale,-3*scale,scale};
        t.document->set_local_transform(parent,p).value(); t.document->set_parent(t.entity,parent).value(); Trsd child; child.rotation=Quatd{Eigen::AngleAxisd{.4,Vec3d::UnitZ()}};
        t.document->set_local_transform(t.entity,child).value(); auto cpu=t.scene(); auto gpu=take(renderer.prepare_scene(queue,cpu));
        std::vector<PathTraceRay> rays;
        for (int i=0;i<35;++i) rays.push_back({{(2+(i%7-3)*.29)*scale,(-3+(i/7-2)*.31)*scale,2*scale},{0,0,-1}});
        auto image=complete(queue,take(renderer.submit_rays(queue,gpu,rays,settings(),{0,1})));
        for (std::size_t i=0;i<rays.size();++i) {
            const auto hit=take(cpu.nearest(rays[i])); const auto& actual=image.primary_hits()[i]; require(bool(hit)==bool(actual),"affine hit identity mismatch");
            if (hit) { require(actual->entity==hit->entity && actual->mesh==hit->mesh && actual->triangle==hit->triangle,"hit provenance mismatch");
                require(std::abs(actual->distance-hit->distance)<=1e-4*std::max(1.0,hit->distance),"distance mismatch");
                require((actual->barycentric-hit->barycentric).norm()<1e-4,"barycentric mismatch"); close_color(actual->normal,hit->normal); }
        }
        t.asset.materials[0].double_sided=false; auto single=take(renderer.prepare_scene(queue,t.scene()));
        const PathTraceRay center{{2*scale,-3*scale,2*scale},{0,0,-1}};
        require(!ray_image(renderer,queue,single,settings(),1,center).primary_hits()[0],"negative winding single-sided backface hit");
        Fixture lit{heap}; lit.quad(-8,8,0,{.6f,.6f,.6f}); lit.quad(-1,1,2,{0,0,0},{2,2,2},true);
        Trsd transform; transform.scale=Vec3d::Constant(scale); transform.translation={2*scale,-3*scale,scale}; lit.document->set_local_transform(lit.entity,transform).value();
        auto light_source=lit.scene(); auto light_gpu=take(renderer.prepare_scene(queue,light_source));
        const auto expected_light=take(estimate_path_reference(light_source,center,settings(),{0,16384}));
        close_color(ray_image(renderer,queue,light_gpu,settings(),16384,center).linear_rgb()[0],expected_light.mean);
    }
    std::puts("analytic/empty/emission/furnace/sun/shadow/affine/scale=pass");
}
struct Population { double mean=0,error=0,mse=0,cpu_error=0; };
Population population(PathTraceRenderer& renderer,SubmissionQueue& queue,const PathTraceGpuScene& scene,PathTraceSettings s,std::uint32_t count,double expected,bool cpu=false) {
    Population result; std::array<double,8> values{}; double difference=0,reference=0;
    for (std::uint32_t i=0;i<8;++i) {
        s.seed=101+7919*i; const auto image=ray_image(renderer,queue,scene,s,count); const auto value=image.linear_rgb()[0]; values[i]=value.x();
        result.mean+=value.x()/8; result.mse+=(value.x()-expected)*(value.x()-expected)/8;
        if (cpu) { const auto oracle=take(estimate_path_reference(scene.source(),down,s,{0,count})).mean; difference+=(value-oracle).squaredNorm(); reference+=oracle.squaredNorm(); }
    }
    for (const auto value:values) result.error+=(value-result.mean)*(value-result.mean)/56;
    result.error=std::sqrt(result.error); result.cpu_error=std::sqrt(difference/std::max(reference,1e-12)); return result;
}
void statistical(Population p,double expected) {
    std::printf("mean=%.9g expected=%.9g stderr=%.9g MSE=%.9g cpu_nRMSE=%.9g\n",p.mean,expected,p.error,p.mse,p.cpu_error);
    require(std::abs(p.mean-expected)<=.02*std::abs(expected)+1e-4,"population bias exceeds 2%");
    require(std::abs(p.mean-expected)<=5*p.error+1e-4,"population outside confidence threshold"); require(p.cpu_error<=.02,"CPU/GPU nRMSE exceeds 2%");
}
double area_integral(unsigned n) {
    double result=0,dx=2.0/n;
    for(unsigned y=0;y<n;++y) for(unsigned x=0;x<n;++x) { double a=-1+(x+.5)*dx,b=-1+(y+.5)*dx,r2=a*a+b*b+4; result+=4/(r2*r2)*dx*dx; }
    return double(.6f)*2*result/std::numbers::pi;
}
void statistics(memory::ResourceHandle heap,PathTraceRenderer& renderer,SubmissionQueue& queue) {
    Fixture f{heap}; f.quad(-8,8,0,{.6f,.6f,.6f}); f.quad(-1,1,2,{0,0,0},{2,2,2},true);
    const auto scene=take(renderer.prepare_scene(queue,f.scene())); auto s=settings(); const double expected=area_integral(256);
    require(std::abs(area_integral(128)-expected)/expected<.001,"quadrature unconverged");
    const auto n=population(renderer,queue,scene,s,16384,expected,true),four=population(renderer,queue,scene,s,65536,expected,true);
    statistical(n,expected); statistical(four,expected); require(std::abs(n.mean-four.mean)/expected<.005,"N/4N reference unconverged");
    s.estimator=PathTraceEstimator::bsdf_only; statistical(population(renderer,queue,scene,s,16384,expected,true),expected); s.estimator=PathTraceEstimator::mis;
    const auto low=population(renderer,queue,scene,s,64,expected),medium=population(renderer,queue,scene,s,256,expected),high=population(renderer,queue,scene,s,1024,expected);
    require(low.mse>medium.mse && medium.mse>high.mse,"64/256/1024 convergence failed");
    std::printf("MSE64/256/1024=%.9g/%.9g/%.9g\n",low.mse,medium.mse,high.mse);
    Fixture box{heap}; box.quad(-1000,1000,0,{.8f,.2f,.1f}); box.quad(-1000,1000,2,{.5f,.5f,.5f},{1,1,1},true);
    const auto indirect=take(renderer.prepare_scene(queue,box.scene())); s=settings(4); s.estimator=PathTraceEstimator::bsdf_only;
    const Vec3d a=Vec3f{.8f,.2f,.1f}.cast<double>(),energy=a.cwiseProduct(Vec3d::Ones()+a*.5);
    close_color(ray_image(renderer,queue,indirect,s,1024).linear_rgb()[0],energy);
    s.rr_start=1; statistical(population(renderer,queue,indirect,s,8192,energy.x(),true),energy.x());
    s.rr_start=3; statistical(population(renderer,queue,indirect,s,8192,energy.x(),true),energy.x());
    for (int combination=0;combination<4;++combination) {
        const bool sky=combination!=2,solar=combination!=1,area=combination!=0; Fixture mixed{heap}; mixed.quad(-8,8,0,{.6f,.6f,.6f});
        if (area) mixed.quad(-1,1,2,{0,0,0},{2,2,2},true);
        EnvironmentDescription e; e.sun.direction={std::sqrt(.8),0,std::sqrt(.2)}; e.sun.irradiance={3,3,3}; mixed.document->set_environment(e).value();
        auto gpu=take(renderer.prepare_scene(queue,mixed.scene())); s=settings(); s.environment_rgb=Vec3d::Constant(sky ? .5 : 0); s.sun=solar ? PathTraceSun::scene : PathTraceSun::none;
        const double mixed_energy=(sky ? double(.6f)*.5 : 0)+(area ? expected*(sky ? .75 : 1) : 0)+(solar ? double(.6f)*3*std::sqrt(.2)/std::numbers::pi : 0);
        statistical(population(renderer,queue,gpu,s,8192,mixed_energy,true),mixed_energy); s.estimator=PathTraceEstimator::bsdf_only;
        statistical(population(renderer,queue,gpu,s,8192,mixed_energy,true),mixed_energy);
    }
    std::puts("NEE/MIS/RR/mixed lights/indirect/convergence=pass");
}
double nrmse(std::span<const Vec3d> a,std::span<const Vec3d> b) { double error=0,base=0; for(std::size_t i=0;i<a.size();++i) { error+=(a[i]-b[i]).squaredNorm(); base+=b[i].squaredNorm(); } return std::sqrt(error/std::max(base,1e-12)); }
void images(memory::ResourceHandle heap,PathTraceRenderer& renderer,SubmissionQueue& queue) {
    Fixture f{heap}; f.quad(-8,8,0,{.6f,.6f,.6f}); f.quad(-1,1,2,{0,0,0},{2,2,2},true);
    const auto source=f.source(); const std::array assets{static_cast<const CpuAsset*>(&f.asset)}; const auto cpu=take(PathTraceScene::create(heap,source,assets));
    const auto scene=take(renderer.prepare_scene(queue,cpu)); auto view=f.view(source); auto s=settings();
    std::array<Vec3d,16> average{},oracle{},referenceN{},reference4N{};
    average.fill(Vec3d::Zero()); oracle.fill(Vec3d::Zero()); referenceN.fill(Vec3d::Zero()); reference4N.fill(Vec3d::Zero());
    for (std::uint32_t seed=0;seed<8;++seed) {
        s.seed=101+seed*7919;
        auto gpu=complete(queue,take(renderer.submit_view(queue,scene,view,s,{0,4096})));
        const auto ref=take(render_path_reference(heap,cpu,view,s,{0,4096})),high=take(render_path_reference(heap,cpu,view,s,{0,16384}));
        require(nrmse(gpu.linear_rgb(),ref.linear_rgb())<=.02,"image CPU/GPU seed mismatch");
        for (std::size_t i=0;i<16;++i) { average[i]+=gpu.linear_rgb()[i]/8; oracle[i]+=ref.linear_rgb()[i]/8; referenceN[i]+=ref.linear_rgb()[i]/8; reference4N[i]+=high.linear_rgb()[i]/8; }
    }
    const auto error=nrmse(average,oracle),convergence=nrmse(referenceN,reference4N);
    std::printf("image 8-seed CPU/GPU nRMSE=%.9g CPU N/4N=%.9g\n",error,convergence);
    require(error<=.02 && convergence<=.005,"image reference or GPU error threshold failed");
    s.seed=42; const auto whole=complete(queue,take(renderer.submit_view(queue,scene,view,s,{0,256})));
    const auto first=complete(queue,take(renderer.submit_view(queue,scene,view,s,{0,100}))),last=complete(queue,take(renderer.submit_view(queue,scene,view,s,{100,156})));
    const auto repeat=complete(queue,take(renderer.submit_view(queue,scene,view,s,{0,256})));
    for(std::size_t i=0;i<16;++i) {
        close_color(whole.linear_rgb()[i],(first.linear_rgb()[i]*100+last.linear_rgb()[i]*156)/256,1e-6);
        require(whole.linear_rgb()[i]==repeat.linear_rgb()[i],"same input image is not deterministic");
        const auto ray=take(path_trace_camera_ray(view,std::uint32_t(i%4),std::uint32_t(i/4),{path_trace_sample(42,std::uint32_t(i),0,0,0),path_trace_sample(42,std::uint32_t(i),0,0,1)}));
        const auto hit=take(cpu.nearest(ray)); require(bool(hit)==bool(whole.primary_hits()[i]),"camera jitter hit mismatch");
        if(hit) require((hit->barycentric-whole.primary_hits()[i]->barycentric).norm()<1e-4,"camera jitter barycentric mismatch");
    }
    s.seed=(1ull<<44)|123; const auto counter=complete(queue,take(renderer.submit_rays(queue,scene,std::span{&down,1},s,{29,512},17)));
    const auto counter_reference=take(estimate_path_reference(cpu,down,s,{29,512},17)); close_color(counter.linear_rgb()[0],counter_reference.mean,1e-4);
    require(counter.info().fixed_rays && counter.info().first_pixel==17 && counter.info().settings.seed==s.seed,"ray counter provenance missing");
    std::ofstream report("pathtrace-hdr.json"); report.precision(17); report<<"{\"schema\":1,\"width\":4,\"height\":4,\"seed\":42,\"spp\":256,\"comparison\":{\"seed_count\":8,\"seed_first\":101,\"seed_stride\":7919,\"samples_n\":4096,\"samples_4n\":16384,\"cpu_gpu_nrmse\":"<<error<<",\"reference_n_4n\":"<<convergence<<"},\"linear_rgb\":[";
    for(std::size_t i=0;i<16;++i) { if(i) report<<','; const auto c=whole.linear_rgb()[i]; report<<'['<<c.x()<<','<<c.y()<<','<<c.z()<<']'; } report<<"]}\n"; require(bool(report),"HDR report write failed");
    std::puts("image/camera/provenance/interval/repeat/HDR=pass");
}
void lifetime(memory::ResourceHandle heap,PathTraceRenderer& renderer,SubmissionQueue& queue,Device& view) {
    Fixture f{heap}; f.quad(-2,2,0,{.5f,.5f,.5f}); auto source=f.scene(); auto gpu=take(renderer.prepare_scene(queue,source)); auto s=settings(); s.environment_rgb=Vec3d::Ones();
    const auto old=ray_image(renderer,queue,gpu,s,16); const auto old_value=old.linear_rgb()[0]; const auto before=queue.stats().submitted;
    const auto old_fingerprint=source.fingerprint(); const auto compatible_source=f.source();
    s.max_rays=1; require(!renderer.submit_rays(queue,gpu,std::span{&down,1},s,{0,16}),"work budget accepted"); s=settings(); s.environment_rgb=Vec3d::Ones();
    require(!renderer.submit_rays(queue,gpu,std::span{&down,1},s,{65535,2}),"sample overflow accepted");
    std::stop_source stop; stop.request_stop(); require(!renderer.prepare_scene(queue,source,stop.get_token()),"cancelled scene accepted");
    require(!renderer.submit_rays(queue,gpu,std::span{&down,1},s,{0,1},0,stop.get_token()),"cancelled frame accepted");
    require(!PathTraceRenderer::create(heap,queue,"missing-shader-directory"),"missing shader accepted");
    require(queue.stats().submitted==before,"rejected request submitted work");
    { auto foreign=take(SubmissionQueue::create(heap,take(view.share_queue(0)),1));
      require(!renderer.prepare_scene(foreign,source),"foreign prepare accepted"); require(!renderer.submit_rays(foreign,gpu,std::span{&down,1},s,{0,1}),"foreign trace accepted"); check(foreign.close()); }
    f.asset.materials[0].emissive={.1f,0,0}; const auto failed_source=f.scene(); f.asset.materials[0].emissive.setZero();
    fail_submit=true; require(!renderer.prepare_scene(queue,failed_source),"failed AS submit published scene");
    require(queue.stats().submitted==before,"AS failure advanced submission"); fail_submit=true;
    require(!renderer.submit_rays(queue,gpu,std::span{&down,1},s,{0,1}),"failed trace submit published frame"); require(queue.stats().submitted==before,"trace failure advanced submission");
    close_color(old.linear_rgb()[0],old_value,0);
    Fixture collapsed{heap}; collapsed.triangle({0,0,0},{.001f,0,0},{0,.001f,0},{.5f,.5f,.5f});
    Trsd shifted; shifted.translation={100000,100000,0}; collapsed.document->set_local_transform(collapsed.entity,shifted).value();
    require(!renderer.prepare_scene(queue,collapsed.scene()),"float-collapsed geometry accepted");
    shifted.translation.setZero(); shifted.scale=Vec3d::Constant(1e-20); collapsed.document->set_local_transform(collapsed.entity,shifted).value();
    require(!renderer.prepare_scene(queue,collapsed.scene()),"unrepresentable triangle area accepted");
    require(queue.stats().submitted==before,"invalid float geometry submitted work");
    auto large_view=f.view(compatible_source).description(); const double near=1e40,far=1e41;
    large_view.projection(2,2)=far/(near-far); large_view.projection(2,3)=far*near/(near-far);
    require(!RenderView::create(compatible_source,large_view),"ill-conditioned camera passed RenderView admission");
    static_cast<void>(f.document->create_entity().value());
    require(!renderer.submit_view(queue,gpu,f.view(f.source()),s,{0,1}),"mismatched scene/view accepted");
    EnvironmentDescription medium; medium.enabled=true; f.document->set_environment(medium).value();
    auto media=take(renderer.prepare_scene(queue,f.scene())); const auto after_media=queue.stats().submitted;
    require(!renderer.submit_rays(queue,media,std::span{&down,1},s,{0,1}),"implicit medium omission accepted");
    require(queue.stats().submitted==after_media,"invalid medium request submitted work");
    require(take(queue.wait(media.submission())),"media build wait failed");
    media={}; medium.enabled=false; f.document->set_environment(medium).value();
    // Reset source to the same revision before checking an asset-only change.
    source=f.scene();
    f.asset.materials[0].emissive={1,2,3}; auto changed=f.scene(); require(changed.fingerprint()!=source.fingerprint() && changed.revision()==source.revision(),"asset-only content identity not frozen");
    auto next=take(renderer.prepare_scene(queue,changed)); const auto updated=ray_image(renderer,queue,next,s,1);
    require(updated.info().fingerprint==changed.fingerprint() && old.info().fingerprint==old_fingerprint,"frame provenance mutated");
    auto frame=take(renderer.submit_rays(queue,next,std::span{&down,1},s,{0,64})); const auto ticket=frame.submission();
    timeout_once=true; require(!take(frame.read(queue)),"forced pending frame published"); require(!frame.read(queue,stop.get_token()),"cancelled read published");
    gpu={}; next={}; renderer={}; f.asset={}; source={}; changed={};
    require(queue.stats().pending_slots>0,"pending work lost owners"); check(queue.close()); require(take(frame.read(queue)).has_value(),"close failed to complete retained frame");
    close_color(old.linear_rgb()[0],old_value,0); require(take(queue.wait(ticket,0)),"closed queue lost ticket");
    std::puts("budget/cancel/compile/AS-submit/trace-submit/foreign/provenance/pending/close=pass");
}
void allocations(memory::MemorySystem& system,memory::ResourceHandle heap,SubmissionQueue& queue) {
    const auto bounded=system.create_heap({"path-failures",memory::DomainCategory::render,16*1024*1024}).value();
    {
        auto renderer=take(PathTraceRenderer::create(bounded,queue,DK_PATH_SHADER_DIR)); Fixture f{heap}; f.quad(-2,2,0,{.5f,.5f,.5f});
        if(queue.device().sampled_image_indexing_enabled()) {
            TextureData texture; texture.id=AssetId::generate().value(); texture.width=texture.height=2; texture.rgba8.resize(16,std::byte{255});
            for(std::size_t i=0;i<2;++i) { f.asset.materials[i].base_color_texture=texture.id; f.asset.mesh.primitives[i].texcoords={{0,0},{1,0},{0,1}}; }
            f.asset.textures.push_back(std::move(texture));
        }
        auto source=f.scene(); auto scene=take(renderer.prepare_scene(queue,source)); auto s=settings(); s.environment_rgb=Vec3d::Ones();
        const auto frame=take(renderer.submit_rays(queue,scene,std::span{&down,1},s,{0,4})); const auto old=complete(queue,frame);
        f.asset.materials[0].emissive={.1f,0,0}; const auto changed=f.scene(); // Force a real upload, not a cache hit.
        struct Block { void* pointer; std::size_t size; };
        struct Blocks : std::vector<Block> {
            memory::ResourceHandle heap;
            ~Blocks() { for(const auto& block:*this) heap.deallocate(block.pointer,block.size); }
        } blocks; blocks.heap=bounded;
        for(const std::size_t bytes:{65536u,256u}) while(auto block=bounded.try_allocate(bytes)) blocks.push_back({*block,bytes});
        bool scene_failed=false,frame_failed=false,read_failed=false,partial=false,success=false;
        for (;;) {
            const auto before=bounded.snapshot(); const auto submitted=queue.stats().submitted; bool prepared=false;
            {
                auto candidate=renderer.prepare_scene(queue,changed); scene_failed|=!candidate; prepared=bool(candidate);
                if (candidate) require(take(queue.wait(candidate->submission())),"candidate wait failed");
                auto render=renderer.submit_rays(queue,scene,std::span{&down,1},s,{0,1}); frame_failed|=!render;
                if (render) require(take(queue.wait(render->submission())),"frame wait failed");
                auto image=frame.read(queue); read_failed|=!image;
                if (!candidate || !render || !image) partial|=bounded.snapshot().allocation_count>before.allocation_count+1;
                if (!candidate && !render) require(queue.stats().submitted==submitted,"allocation failure submitted work");
                success=bool(candidate) && bool(render) && bool(image);
            }
            // A successful prepare can replace the bounded weak-cache control blocks. Failed prepares cannot.
            if(!prepared) require(bounded.snapshot().live_allocations==before.live_allocations && bounded.snapshot().backing_requested_bytes==before.backing_requested_bytes,"failed candidate leaked Memory");
            require(old.info().fingerprint==source.fingerprint() && old.linear_rgb().size()==1,"old image changed during allocation failure");
            if(success || blocks.empty()) break;
            auto block=blocks.back(); blocks.pop_back(); bounded.deallocate(block.pointer,block.size);
        }
        for (const auto& block:blocks) bounded.deallocate(block.pointer,block.size); blocks.clear();
        require(scene_failed && frame_failed && read_failed && partial && success,"allocation test missed failure/recovery path");
        bounded.begin_close(); require(!renderer.prepare_scene(queue,source) && !frame.read(queue),"closing Memory accepted new work"); require(old.linear_rgb().size()==1,"closing invalidated old image");
    }
    require(bounded.snapshot().live_allocations==0,"bounded Memory leaked"); std::puts("initial/mid-allocation/old-frame/recovery/closing=pass");
}
}
#include "PathTextureCases.hpp"
#include "PathGgxCases.hpp"
#include "PathProgressiveCases.hpp"
#include "PathAirCases.hpp"
#include "PathCloudCases.hpp"
#include "PathOrthographicCases.hpp"
#include "PathRestirCases.hpp"
#include "PathTemporalCases.hpp"
#include "PathSpatialCases.hpp"
#include "PathRestirMediumCases.hpp"
#include "PathRestirMediumSupportCases.hpp"
#include "PathTileGroupCases.hpp"
int main(int argc,char** argv) {
    try {
        require(argc==2,"expected analytic/statistics/images/lifetime case");
        struct Memory { memory::MemorySystem system=memory::MemorySystem::create().value();
            memory::ResourceHandle heap=system.create_heap({"path-probe",memory::DomainCategory::render}).value(); } memory;
        Diagnostics diagnostics;
        {
            memory::ThreadContext context{memory.system,memory.heap}; memory::ExecutionScope scope{context,memory.heap};
            DeviceOptions options; options.validation=ValidationMode::required; options.ray_tracing=RayTracingMode::required;
            const std::string_view requested=argv[1];
            if((requested.starts_with("texture_") && requested!="texture_disabled") || requested=="ggx_lifetime" || requested=="air_resources" || requested=="cloud_surface" || requested=="restir_textures" || requested=="restir_temporal_schedule" || requested=="restir_spatial_schedule" || requested=="restir_medium_lights" || requested=="restir_medium_schedule") options.sampled_image_indexing=SampledImageIndexingMode::required;
            options.diagnostic_sink=diagnostic; options.diagnostic_user_data=&diagnostics; auto device=take(Device::create(memory.heap,options)); auto view=take(device.share_queue(0));
            std::printf("GPU=%s validation=%d case=%s\n",device.adapter().properties.deviceName.data(),device.validation_enabled(),argv[1]);
            native_submit=reinterpret_cast<PFN_vkQueueSubmit2>(device.device_proc("vkQueueSubmit2")); native_wait=reinterpret_cast<PFN_vkWaitSemaphores>(device.device_proc("vkWaitSemaphores"));
            auto queue=take(graphics::detail::SubmissionAccess::create(memory.heap,std::move(device),3,{submit_override,nullptr,wait_override}));
            { auto renderer=take(PathTraceRenderer::create(memory.heap,queue,DK_PATH_SHADER_DIR));
              const std::string_view mode=argv[1];
              if(mode=="restir_medium_numeric") restir_medium_numeric(memory.heap,renderer,queue);
              else if(mode=="restir_medium_lights") restir_medium_lights(memory.heap,renderer,queue);
              else if(mode=="restir_medium_cloud") restir_medium_cloud(memory.heap,renderer,queue);
              else if(mode=="restir_medium_reuse") restir_medium_reuse(memory.heap,renderer,queue);
              else if(mode=="restir_medium_schedule") restir_medium_schedule(memory.heap,renderer,queue);
              else if(mode=="restir_medium_state") restir_medium_state(memory.heap,renderer,queue);
              else if(mode=="restir_medium_failure") restir_medium_failure(memory.heap,queue);
              else if(mode=="restir_medium_support") restir_medium_support(memory.heap,renderer,queue);
              else if(mode=="restir_medium_support_initial") restir_medium_support(memory.heap,renderer,queue,true);
              else if(mode=="restir_spatial_numeric") spatial_numeric(memory.heap,renderer,queue);
              else if(mode=="restir_spatial_schedule") spatial_schedule(memory.heap,renderer,queue);
              else if(mode=="restir_spatial_state") spatial_state(memory.heap,renderer,queue);
              else if(mode=="restir_spatial_support") spatial_support(memory.heap,renderer,queue,false);
              else if(mode=="restir_spatial_edges") spatial_support(memory.heap,renderer,queue,true);
              else if(mode=="restir_spatial_budget") spatial_budget(memory.heap,queue);
              else if(mode=="restir_temporal_numeric") temporal_numeric(memory.heap,renderer,queue);
              else if(mode=="restir_temporal_materials") temporal_materials(memory.heap,renderer,queue);
              else if(mode=="restir_temporal_schedule") temporal_schedule(memory.heap,renderer,queue);
              else if(mode=="restir_temporal_state") temporal_state(memory.heap,renderer,queue);
              else if(mode=="restir_temporal_boundaries") temporal_boundaries(memory.heap,renderer,queue);
              else if(mode=="restir_temporal_support") temporal_support(memory.heap,renderer,queue);
              else if(mode=="restir_numeric") restir_numeric(memory.heap,renderer,queue);
              else if(mode=="restir_materials") restir_materials(memory.heap,renderer,queue);
              else if(mode=="restir_textures") restir_textures(memory.heap,renderer,queue);
              else if(mode=="restir_schedule") restir_schedule(memory.heap,renderer,queue);
              else if(mode=="restir_state") restir_state(memory.heap,renderer,queue);
              else if(mode=="orthographic") orthographic(memory.heap,renderer,queue);
              else if(mode=="orthographic_medium") orthographic_medium(memory.heap,renderer,queue);
              else if(mode=="cloud_numeric") cloud_numeric(memory.heap,renderer,queue);
              else if(mode=="cloud_statistics") cloud_statistics(memory.heap,renderer,queue);
              else if(mode=="cloud_surface") cloud_surface(memory.heap,renderer,queue);
              else if(mode=="cloud_progressive") cloud_progressive(memory.heap,renderer,queue);
              else if(mode=="cloud_failure") cloud_failure(memory.heap,queue);
              else if(mode=="air_numeric") air_numeric(memory.heap,renderer,queue);
              else if(mode=="air_statistics") air_statistics(memory.heap,renderer,queue);
              else if(mode=="air_progressive") air_progressive(memory.heap,renderer,queue);
              else if(mode=="air_failure") air_failure(memory.heap,renderer,queue);
              else if(mode=="air_resources") air_resources(memory.heap,queue);
              else if(mode=="tile_group_images") tile_group_images(memory.heap,renderer,queue);
              else if(mode=="tile_group_state") tile_group_state(memory.heap,renderer,queue);
              else if(mode=="progressive_images") progressive_images(memory.heap,renderer,queue);
              else if(mode=="progressive_state") progressive_state(memory.heap,renderer,queue);
              else if(mode=="progressive_failure") progressive_failure(memory.heap,renderer,queue);
              else if(mode=="progressive_lost") progressive_lost(memory.heap,view);
              else if(mode=="progressive_budget") progressive_budget(memory.heap,renderer,queue);
              else if(mode=="analytic") analytic(memory.heap,renderer,queue);
              else if(mode=="statistics") statistics(memory.heap,renderer,queue);
              else if(mode=="images") images(memory.heap,renderer,queue);
              else if(mode=="lifetime") lifetime(memory.heap,renderer,queue,view);
              else if(mode=="allocations") allocations(memory.system,memory.heap,queue);
              else if(mode=="ggx_numeric") ggx_numeric(memory.heap,renderer,queue);
              else if(mode=="ggx_statistics") ggx_statistics(memory.heap,renderer,queue);
              else if(mode=="ggx_indirect") ggx_indirect(memory.heap,renderer,queue);
              else if(mode=="ggx_lifetime") ggx_lifetime(memory.heap,renderer,queue);
              else if(mode=="texture_numeric") texture_numeric(memory.heap,renderer,queue);
              else if(mode=="texture_mask") texture_mask(memory.heap,renderer,queue);
              else if(mode=="texture_lifetime") texture_lifetime(memory.heap,renderer,queue);
              else if(mode=="texture_disabled") texture_disabled(memory.heap,renderer,queue);
              else if(mode=="texture_allocations") allocations(memory.system,memory.heap,queue);
#ifdef DK_PATH_ASSET_DIR
              else if(mode=="texture_assets") texture_assets(memory.heap,renderer,queue);
#endif
              else throw std::runtime_error("unknown case"); }
            check(queue.close()); VmaTotalStatistics statistics{}; vmaCalculateStatistics(view.allocator(),&statistics);
            require(statistics.total.statistics.allocationCount==0,"GPU allocation leak");
        }
        std::printf("errors=%u warnings=%u liveAllocations=%zu\n",diagnostics.errors.load(),diagnostics.warnings.load(),memory.heap.snapshot().live_allocations);
        require(!diagnostics.errors && !diagnostics.warnings && !memory.heap.snapshot().live_allocations,"validation or Memory leak");
        memory.heap.begin_close(); require(memory.system.try_close().closed(),"Memory close failed"); return 0;
    } catch(const std::exception& error) { std::fprintf(stderr,"required path trace: %s\n",error.what()); return 1; }
}
