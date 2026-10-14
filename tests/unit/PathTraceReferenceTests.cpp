#include "../RenderAssetSupport.hpp"
#include <dk/render/PathTraceReference.hpp>
#include <dk/scene/SceneDocument.hpp>
#include <dk/geometry/Query.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <array>
#include <chrono>
#include <iostream>
#include <numbers>
#include <thread>

using namespace dk;
using namespace dk::render;
namespace {
struct Fixture : RenderMemory {
    std::unique_ptr<SceneDocument> document=SceneDocument::create().value();
    CpuAsset asset;
    EntityId entity;
    Fixture() {
        asset.mesh.id=AssetId::generate().value(); entity=document->create_entity().value();
        document->set_asset_references(entity,{{asset.mesh.id,AssetKind::mesh}}).value();
    }
    void triangle(Vec3f a,Vec3f b,Vec3f c,Vec3f albedo={.6f,.6f,.6f},Vec3f emission={0,0,0},bool two_sided=false) {
        MaterialData m; m.id=AssetId::generate().value(); m.metallic=0; m.base_color={albedo.x(),albedo.y(),albedo.z(),1};
        m.emissive=emission; m.double_sided=two_sided; asset.materials.push_back(m);
        MeshPrimitive p; p.material=m.id; p.positions={a,b,c}; p.indices={0,1,2}; asset.mesh.primitives.push_back(std::move(p));
    }
    void quad(float lo,float hi,float z,Vec3f albedo,Vec3f emission={0,0,0},bool flip=false) {
        const Vec3f a{lo,lo,z},b{hi,lo,z},c{hi,hi,z},d{lo,hi,z};
        if (flip) { triangle(a,c,b,albedo,emission); triangle(a,d,c,albedo,emission); }
        else { triangle(a,b,c,albedo,emission); triangle(a,c,d,albedo,emission); }
    }
    RenderScene source() { return RenderScene::extract(heap,document->snapshot().value()).value(); }
    PathTraceScene scene() { const std::array assets{static_cast<const CpuAsset*>(&asset)}; return PathTraceScene::create(heap,source(),assets).value(); }
    RenderView view(const RenderScene& source,std::uint32_t size=8) {
        ViewDescription d{size,size}; Trsd camera; camera.translation={0,0,1}; d.camera_world=Transformd::from_trs(camera).value();
        d.projection.setZero(); d.projection(0,0)=2; d.projection(1,1)=-2;
        const double near=.1,far=10; d.projection(2,2)=far/(near-far); d.projection(2,3)=far*near/(near-far); d.projection(3,2)=-1;
        return RenderView::create(source,d).value();
    }
};
const PathTraceRay down{{0,0,1},{0,0,-1}};
PathTraceSettings settings(std::uint32_t depth=1) { PathTraceSettings s; s.max_bounces=depth; s.rr_start=0; return s; }
struct Population {
    double mean=0,standard_error=0,mse=0;
};
Population population(const PathTraceScene& scene,PathTraceSettings s,std::uint32_t count,double expected,std::uint32_t first=0) {
    std::array<double,8> values{}; Population p;
    for (std::uint32_t i=0;i<values.size();++i) {
        s.seed=101+7919*i;
        auto estimate=estimate_path_reference(scene,down,s,{first,count}); REQUIRE(estimate);
        values[i]=estimate->mean.x(); p.mean+=values[i]/values.size();
        p.mse+=(values[i]-expected)*(values[i]-expected)/values.size();
    }
    for (const double v:values) p.standard_error+=(v-p.mean)*(v-p.mean)/(values.size()*(values.size()-1));
    p.standard_error=std::sqrt(p.standard_error); return p;
}
void statistical_check(Population p,double expected) {
    INFO("mean="<<p.mean<<" expected="<<expected<<" stderr="<<p.standard_error);
    CHECK(std::abs(p.mean-expected)<=.02*std::abs(expected)+1e-4);
    CHECK(std::abs(p.mean-expected)<=5*p.standard_error+1e-4);
}
double area_integral(unsigned steps) {
    double total=0; const double dx=2.0/steps;
    for (unsigned y=0;y<steps;++y) for (unsigned x=0;x<steps;++x) {
        const double px=-1+(x+.5)*dx,py=-1+(y+.5)*dx,r2=px*px+py*py+4;
        total+=4/(r2*r2)*dx*dx;
    }
    return double(.6f)*2*total/std::numbers::pi;
}
}

TEST_CASE("counter samples have frozen sequence range and independent dimensions") {
    // Golden sequence of the v1 24-bit counter output, not reimplemented by the test.
    const std::array<std::uint32_t,9> golden{1195510,2764145,16368731,4388082,11615314,4980972,13037384,6438060,6814687};
    for (std::uint32_t d=0;d<9;++d) CHECK(path_trace_sample(1,17,29,3,d)==(double(golden[d])+.5)/16777216.0);
    double sum=0;
    for (std::uint32_t i=0;i<10000;++i) {
        const double u=path_trace_sample(1,0,i,0,6); REQUIRE(u>0); REQUIRE(u<1); sum+=u;
    }
    CHECK(sum/10000==Catch::Approx(.5).margin(.015));
    CHECK(path_trace_sample(1,0,0,0,0)!=path_trace_sample(1ull<<32,0,0,0,0));
}
TEST_CASE("world triangles agree with independent BVH under shear reflection and scale") {
    Fixture f; f.triangle({-1,-1,0},{1,-1,0},{0,1,0},{.5f,.5f,.5f},{0,0,0},true);
    const auto parent=f.document->create_entity().value(); Trsd p; p.scale={-2,3,.5};
    f.document->set_local_transform(parent,p).value(); f.document->set_parent(f.entity,parent).value();
    Trsd child; child.rotation=Quatd{Eigen::AngleAxisd{.4,Vec3d::UnitZ()}}; child.translation={.2,.1,0};
    f.document->set_local_transform(f.entity,child).value(); auto scene=f.scene();
    const std::array triangles{geometry::Triangle{{-1,-1,0},{1,-1,0},{0,1,0}}};
    auto bvh=geometry::MeshQuery::build(triangles).value(); const auto world=f.document->world_transform(f.entity).value();
    for (int i=0;i<81;++i) {
        const Vec3d origin{(i%9-4)*.31,(i/9-4)*.33,2};
        const auto expected=bvh.nearest({origin,{0,0,-1}},world).value(); const auto actual=scene.nearest({origin,{0,0,-1}}).value();
        REQUIRE(bool(actual)==bool(expected));
        if (actual) { CHECK(actual->entity==f.entity); CHECK(actual->distance==Catch::Approx(expected->distance).margin(1e-10)); CHECK((actual->barycentric-expected->barycentric).norm()<1e-10); }
    }
    f.asset.materials[0].double_sided=false; CHECK_FALSE(f.scene().nearest({{0,0,2},{0,0,-1}}).value());
    p.scale.z()=0; f.document->set_local_transform(parent,p).value(); const std::array inputs{static_cast<const CpuAsset*>(&f.asset)};
    CHECK_FALSE(PathTraceScene::create(f.heap,f.source(),inputs)); CHECK(scene.nearest({{0,0,2},{0,0,-1}}).value());
}
TEST_CASE("scene snapshots freeze consumed assets revision and lifetime") {
    Fixture f; f.triangle({-1,-1,0},{1,-1,0},{0,1,0});
    auto old=f.scene(),repeat=f.scene(); CHECK(old.fingerprint()==repeat.fingerprint());
    f.asset.mesh.primitives[0].positions[0].z()=.5f; auto next=f.scene(); CHECK(old.fingerprint()!=next.fingerprint());
    CHECK(old.revision()==next.revision()); CHECK(old.nearest(down).value()->distance==1);
    const auto revision=f.document->revision(); const auto dirty=f.document->dirty();
    f.asset.mesh.primitives[0].indices[1]=20; const std::array input{static_cast<const CpuAsset*>(&f.asset)};
    CHECK_FALSE(PathTraceScene::create(f.heap,f.source(),input));
    CHECK(f.document->revision()==revision); CHECK(f.document->dirty()==dirty);
    f.document.reset(); f.asset.mesh.primitives.clear(); f.heap.begin_close();
    CHECK(old.nearest(down).value()->distance==1); CHECK(old.triangle_count()==1);
}
TEST_CASE("scale aware offsets avoid self shadows and instance area weights remain physical") {
    const double scale=GENERATE(1e-6,1.0,1e4);
    Fixture f; f.quad(-8,8,0,{.6f,.6f,.6f}); f.quad(-1,1,2,{0,0,0},{2,2,2},true);
    Trsd transform; transform.scale=Vec3d::Constant(scale); transform.translation={2*scale,-3*scale,scale};
    f.document->set_local_transform(f.entity,transform).value(); const auto scene=f.scene();
    const PathTraceRay ray{{2*scale,-3*scale,2*scale},{0,0,-1}};
    auto s=settings(); auto value=estimate_path_reference(scene,ray,s,{0,16384}); REQUIRE(value);
    CHECK(value->mean.x()==Catch::Approx(area_integral(256)).epsilon(.02));
    CHECK(scene.nearest(ray).value()->distance==Catch::Approx(scale).epsilon(1e-10));
    PathTraceRay clipped=ray; clipped.maximum=.5*scale; CHECK_FALSE(scene.nearest(clipped).value());
}
TEST_CASE("empty degenerate missing duplicate inputs and lower scene limits have explicit outcomes") {
    Fixture f; auto source=f.source(); const std::array inputs{static_cast<const CpuAsset*>(&f.asset)};
    REQUIRE(PathTraceScene::create(f.heap,source,inputs)); CHECK_FALSE(PathTraceScene::create(f.heap,source,{}));
    const std::array duplicate{inputs[0],inputs[0]}; CHECK_FALSE(PathTraceScene::create(f.heap,source,duplicate));
    f.triangle({0,0,0},{0,0,0},{0,0,0}); auto degenerate=f.scene(); CHECK(degenerate.triangle_count()==0); CHECK(degenerate.degenerate_count()==1);
    f.triangle({-1,-1,0},{1,-1,0},{0,1,0}); source=f.source();
    CHECK_FALSE(PathTraceScene::create(f.heap,source,inputs,{1,4096}));
    CHECK_FALSE(PathTraceScene::create(f.heap,source,inputs,{4096,1}));
    CHECK_FALSE(PathTraceScene::create(f.heap,source,inputs,{4097,4096}));
    CHECK_FALSE(degenerate.nearest({{0,0,0},{0,0,0}}));
    CHECK_FALSE(PathTraceScene{}.nearest(down));
}
TEST_CASE("unsupported materials malformed assets and scene budgets reject before publication") {
    Fixture f; f.triangle({-1,-1,0},{1,-1,0},{0,1,0}); const auto before=f.scene();
    const auto source=f.source(); const std::array inputs{static_cast<const CpuAsset*>(&f.asset)};
    const int mode=GENERATE(0,1,2,3,4,5,6,7,8,9,10);
    switch(mode) {
    case 0: f.asset.materials[0].metallic=1; break;
    case 1: f.asset.materials[0].alpha_mode=AlphaMode::mask; break;
    case 2: f.asset.materials[0].base_color_texture=AssetId::generate().value(); break;
    case 3: f.asset.mesh.primitives[0].material.reset(); break;
    case 4: f.asset.mesh.primitives[0].indices.push_back(0); break;
    case 5: f.asset.mesh.primitives[0].positions[0].x()=std::numeric_limits<float>::quiet_NaN(); break;
    case 6: f.asset.materials[0].base_color.x()=-1; break;
    case 7: f.asset.materials[0].emissive.x()=std::numeric_limits<float>::infinity(); break;
    case 8: f.asset.materials[0].id={}; break;
    case 9: f.asset.mesh.id={}; break;
    case 10: f.asset.mesh.primitives[0].indices.resize(4097*3,0); break;
    }
    CHECK_FALSE(PathTraceScene::create(f.heap,source,inputs)); CHECK(before.nearest(down).value()->distance==1);
}
TEST_CASE("camera rays preserve Vulkan pixel orientation clipping and reject unsupported projections") {
    Fixture f; auto source=f.source(); auto view=f.view(source,2);
    auto a=path_trace_camera_ray(view,0,0,{.5,.5}).value(),b=path_trace_camera_ray(view,1,1,{.5,.5}).value();
    CHECK(a.direction.x()<0); CHECK(a.direction.y()>0); CHECK(a.direction.z()<0); CHECK(b.direction.x()>0); CHECK(b.direction.y()<0);
    CHECK((a.origin+a.direction*a.minimum).z()==Catch::Approx(.9)); CHECK((a.origin+a.direction*a.maximum).z()==Catch::Approx(-9));
    CHECK_FALSE(path_trace_camera_ray(view,2,0,{.5,.5})); CHECK_FALSE(path_trace_camera_ray(view,0,0,{1,.5}));
    auto d=view.description(); d.projection.setIdentity(); CHECK_FALSE(path_trace_camera_ray(RenderView::create(source,d).value(),0,0,{.5,.5}));
    d=view.description(); d.projection(0,2)=.2; CHECK(path_trace_camera_ray(RenderView::create(source,d).value(),0,0,{.5,.5}));
    d=view.description(); Trsd scaled; scaled.scale={2,1,1}; d.camera_world=Transformd::from_trs(scaled).value();
    CHECK_FALSE(path_trace_camera_ray(RenderView::create(source,d).value(),0,0,{.5,.5}));
}
TEST_CASE("black constant background emission and Lambert white furnace obey analytic radiance") {
    Fixture f; auto s=settings(); auto empty=f.scene();
    CHECK(estimate_path_reference(empty,down,s,{0,1})->mean==Vec3d::Zero());
    s.environment_rgb={.25,.5,.75}; CHECK(estimate_path_reference(empty,down,s,{0,8})->mean==s.environment_rgb);
    f.quad(-10,10,0,{.5f,.25f,1}); auto scene=f.scene(); s.estimator=PathTraceEstimator::bsdf_only;
    const Vec3d expected{.125,.125,.75}; CHECK((estimate_path_reference(scene,down,s,{0,256})->mean-expected).norm()<1e-12);
    s.estimator=PathTraceEstimator::mis; auto p=population(scene,s,16384,expected.x()); statistical_check(p,expected.x());
    for (auto& m:f.asset.materials) m.emissive={2,3,4}; scene=f.scene(); s.environment_rgb.setZero();
    CHECK((estimate_path_reference(scene,down,s,{0,16})->mean-Vec3d{2,3,4}).norm()<1e-12);
    std::cout<<"white furnace mean="<<p.mean<<" stderr="<<p.standard_error<<'\n';
}
TEST_CASE("delta sunlight is analytic and secondary visibility does not inherit camera clipping") {
    Fixture f; f.quad(-10,10,0,{.5f,.25f,1}); EnvironmentDescription env;
    env.sun.direction={std::sqrt(.5),0,std::sqrt(.5)}; env.sun.irradiance={2,3,4}; f.document->set_environment(env).value();
    auto scene=f.scene(); auto s=settings(); s.sun=PathTraceSun::scene;
    const Vec3d expected=Vec3d{.5,.25,1}.cwiseProduct(Vec3d{2,3,4})*(std::sqrt(.5)/std::numbers::pi);
    CHECK((estimate_path_reference(scene,down,s,{0,32})->mean-expected).norm()<1e-12);
    const PathTraceRay clipped{{0,0,.1},{0,0,-1},0,.2};
    f.triangle({.2f,-2,.5f},{2,2,.5f},{2,-2,.5f},{0,0,0},{0,0,0},true);
    f.triangle({.2f,-2,.5f},{.2f,2,.5f},{2,2,.5f},{0,0,0},{0,0,0},true);
    CHECK(estimate_path_reference(f.scene(),clipped,s,{0,16})->mean==Vec3d::Zero());
    s.estimator=PathTraceEstimator::bsdf_only; CHECK((estimate_path_reference(scene,down,s,{0,16})->mean-expected).norm()<1e-12);
}
TEST_CASE("area light MIS and BSDF estimates agree with independent quadrature and converge") {
    Fixture f; f.quad(-8,8,0,{.6f,.6f,.6f}); f.quad(-1,1,2,{0,0,0},{2,2,2},true);
    const auto scene=f.scene(); auto s=settings(); const double expected=area_integral(256);
    CHECK(std::abs(area_integral(128)-expected)/expected<.001);
    const auto a=population(scene,s,16384,expected),b=population(scene,s,65536,expected);
    statistical_check(a,expected); statistical_check(b,expected);
    CHECK(std::abs(a.mean-b.mean)/expected<.005);
    s.estimator=PathTraceEstimator::bsdf_only; auto bsdf=population(scene,s,16384,expected); statistical_check(bsdf,expected);
    s.estimator=PathTraceEstimator::mis;
    auto low=population(scene,s,64,expected),medium=population(scene,s,256,expected),high=population(scene,s,1024,expected);
    CHECK(high.mse<medium.mse); CHECK(medium.mse<low.mse);
    std::cout<<"area quadrature="<<expected<<" MIS N="<<a.mean<<" 4N="<<b.mean<<" BSDF="<<bsdf.mean
        <<" MSE 64/256/1024="<<low.mse<<'/'<<medium.mse<<'/'<<high.mse<<'\n';
}
TEST_CASE("multiple diffuse bounces transfer color and roulette preserves the mean") {
    Fixture f; f.quad(-1000,1000,0,{.8f,.2f,.1f}); f.quad(-1000,1000,2,{.5f,.5f,.5f},{1,1,1},true);
    const auto scene=f.scene(); auto s=settings(4); s.estimator=PathTraceEstimator::bsdf_only;
    const Vec3d a=Vec3f{.8f,.2f,.1f}.cast<double>(), expected=a.cwiseProduct(Vec3d::Ones()+a*.5);
    auto no_rr=estimate_path_reference(scene,down,s,{0,1024}).value(); CHECK((no_rr.mean-expected).norm()<1e-4);
    s.max_bounces=1; CHECK((estimate_path_reference(scene,down,s,{0,64})->mean-a).norm()<1e-4);
    s.max_bounces=4; s.rr_start=1; auto rr=population(scene,s,8192,expected.x()); statistical_check(rr,expected.x());
    s.rr_start=3; auto later=population(scene,s,8192,expected.x()); statistical_check(later,expected.x());
    std::cout<<"indirect expected="<<expected.transpose()<<" RR1="<<rr.mean<<" RR3="<<later.mean<<'\n';
}
TEST_CASE("mixed light classes preserve analytic energy and selection probabilities") {
    const int combination=GENERATE(0,1,2,3);
    const bool sky=combination!=2,solar=combination!=1,area=combination!=0;
    Fixture f; f.quad(-8,8,0,{.6f,.6f,.6f});
    if (area) f.quad(-1,1,2,{0,0,0},{2,2,2},true);
    EnvironmentDescription env; env.sun.direction={std::sqrt(.8),0,std::sqrt(.2)}; env.sun.irradiance={3,3,3};
    f.document->set_environment(env).value();
    auto s=settings(); s.environment_rgb=Vec3d::Constant(sky ? .5 : 0); s.sun=solar ? PathTraceSun::scene : PathTraceSun::none;
    // The panel replaces the environment in its projected solid angle. The oblique sun misses the panel.
    const double expected=(sky ? double(.6f)*.5 : 0)+(area ? area_integral(256)*(sky ? .75 : 1) : 0)+
        (solar ? double(.6f)*3*std::sqrt(.2)/std::numbers::pi : 0);
    const auto scene=f.scene(); statistical_check(population(scene,s,8192,expected),expected);
    s.estimator=PathTraceEstimator::bsdf_only; statistical_check(population(scene,s,8192,expected),expected);
}
TEST_CASE("sample intervals and images are reproducible and immutable") {
    Fixture f; f.quad(-8,8,0,{.6f,.6f,.6f}); f.quad(-1,1,2,{0,0,0},{2,2,2},true);
    const auto source=f.source(); const std::array inputs{static_cast<const CpuAsset*>(&f.asset)};
    const auto scene=PathTraceScene::create(f.heap,source,inputs).value(); auto view=f.view(source); auto s=settings(2);
    auto whole=render_path_reference(f.heap,scene,view,s,{0,256}).value();
    auto first=render_path_reference(f.heap,scene,view,s,{0,100}).value(),last=render_path_reference(f.heap,scene,view,s,{100,156}).value();
    auto repeat=render_path_reference(f.heap,scene,view,s,{0,256}).value();
    for (std::size_t i=0;i<whole.linear_rgb().size();++i) {
        CHECK(whole.linear_rgb()[i]==repeat.linear_rgb()[i]);
        CHECK((whole.linear_rgb()[i]-(first.linear_rgb()[i]*100+last.linear_rgb()[i]*156)/256).norm()<1e-12);
    }
    CHECK(whole.info().fingerprint==scene.fingerprint()); CHECK(whole.info().samples.count==256);
    CHECK(whole.info().statistics.rays<=64*256*5);
    const auto preview=RenderScene::extract(f.heap,f.document->snapshot().value(),LocalTransformOverride{f.entity,Trsd{{1,0,0}}}).value();
    CHECK_FALSE(render_path_reference(f.heap,scene,f.view(preview),s,{0,1}));
    f.heap.begin_close(); CHECK_FALSE(render_path_reference(f.heap,scene,view,s,{0,1})); CHECK(whole.linear_rgb().size()==64);
}
TEST_CASE("budgets invalid inputs media and cancellation preserve previous results") {
    Fixture f; f.quad(-1,1,0,{.5f,.5f,.5f}); auto source=f.source(); const std::array inputs{static_cast<const CpuAsset*>(&f.asset)};
    auto scene=PathTraceScene::create(f.heap,source,inputs).value(); auto view=f.view(source); auto s=settings(); s.environment_rgb=Vec3d::Ones();
    auto old=render_path_reference(f.heap,scene,view,s,{0,2}).value(); const auto before=f.heap.snapshot();
    const int mode=GENERATE(0,1,2,3,4,5,6,7,8);
    switch(mode) {
    case 0: s.max_rays=1; break;
    case 1: s.max_triangle_tests=1; break;
    case 2: s.max_bounces=0; break;
    case 3: s.rr_start=33; break;
    case 4: s.model_version=2; break;
    case 5: s.sampler_version=2; break;
    case 6: s.estimator=static_cast<PathTraceEstimator>(42); break;
    case 7: s.environment_rgb.x()=std::numeric_limits<double>::quiet_NaN(); break;
    case 8: s.max_rays=std::numeric_limits<std::uint64_t>::max(); break;
    }
    CHECK_FALSE(render_path_reference(f.heap,scene,view,s,{0,2})); CHECK(old.linear_rgb().size()==64);
    CHECK(f.heap.snapshot().live_allocations==before.live_allocations);
    s=settings(); CHECK_FALSE(estimate_path_reference(scene,down,s,{65535,2})); CHECK_FALSE(estimate_path_reference(scene,down,s,{0,0}));
    std::stop_source cancel; cancel.request_stop(); auto stopped=render_path_reference(f.heap,scene,view,s,{0,2},cancel.get_token());
    REQUIRE_FALSE(stopped); CHECK(stopped.error().context==std::vector<std::string>{"render.pathtracing.reference.cancelled"});
    EnvironmentDescription env; env.enabled=true; f.document->set_environment(env).value(); scene=f.scene();
    CHECK_FALSE(estimate_path_reference(scene,down,s,{0,1})); s.surface_only=true; REQUIRE(estimate_path_reference(scene,down,s,{0,1}));
}
TEST_CASE("allocation failures unwind scene and image candidates without changing old owners") {
    Fixture f; f.quad(-1,1,0,{.5f,.5f,.5f}); const auto source=f.source(); const std::array inputs{static_cast<const CpuAsset*>(&f.asset)};
    const auto small=f.system.create_heap({"path-budget",memory::DomainCategory::render,32768}).value();
    const auto old=PathTraceScene::create(small,source,inputs).value(); const auto view=f.view(source);
    struct Block { void* p; std::size_t size; }; std::vector<Block> blocks;
    while (auto p=small.try_allocate(256)) blocks.push_back({*p,256});
    bool failed_scene=false,failed_image=false,partial=false,success=false;
    for (;;) {
        const auto before=small.snapshot();
        {
            auto candidate=PathTraceScene::create(small,source,inputs);
            failed_scene|=!candidate;
            auto image=render_path_reference(small,old,view,settings(),{0,1});
            failed_image|=!image; success|=bool(image);
            if (!image) partial|=small.snapshot().allocation_count>before.allocation_count+1;
        }
        CHECK(small.snapshot().live_allocations==before.live_allocations);
        CHECK(small.snapshot().backing_requested_bytes==before.backing_requested_bytes);
        CHECK(old.triangle_count()==2);
        if (blocks.empty()) break;
        auto block=blocks.back(); blocks.pop_back(); small.deallocate(block.p,block.size);
    }
    CHECK(failed_scene); CHECK(failed_image); CHECK(partial); CHECK(success);
}
TEST_CASE("active reference cancellation discards partial image and leaves scene reusable") {
    Fixture f; f.quad(-1000,1000,0,{.8f,.8f,.8f}); f.quad(-1000,1000,2,{.8f,.8f,.8f},{1,1,1},true);
    const auto source=f.source(); const std::array inputs{static_cast<const CpuAsset*>(&f.asset)};
    const auto scene=PathTraceScene::create(f.heap,source,inputs).value(); auto view=f.view(source,16); auto s=settings(8);
    const auto before=f.heap.snapshot(); std::stop_source cancel;
    std::jthread stopper([&] { std::this_thread::sleep_for(std::chrono::milliseconds(10)); cancel.request_stop(); });
    auto result=render_path_reference(f.heap,scene,view,s,{0,2048},cancel.get_token());
    REQUIRE_FALSE(result); CHECK(result.error().context==std::vector<std::string>{"render.pathtracing.reference.cancelled"});
    CHECK(f.heap.snapshot().live_allocations==before.live_allocations);
    REQUIRE(render_path_reference(f.heap,scene,view,settings(),{0,1}));
}
