#include "PipelinePolicy.hpp"
#include <dk/scene/SceneDocument.hpp>
#include <dk/memory/MemorySystem.hpp>
#include <dk/math/Projection.hpp>
#include <catch2/catch_test_macros.hpp>
#include <limits>
using namespace dk;
using namespace dk::render;
namespace {
struct Fixture {
    memory::MemorySystem system=std::move(memory::MemorySystem::create().value());
    memory::ResourceHandle heap=system.create_heap({"pipeline-test",memory::DomainCategory::render}).value();
    RenderScene scene=RenderScene::extract(heap,SceneDocument::create().value()->snapshot().value()).value();
    RenderView view=RenderView::create(scene,{64,64}).value();
};
}
TEST_CASE("pipeline settings reject nonfinite negative and overflowing values") {
    Fixture f; RenderSettings settings;
    CHECK(validate_render_settings(f.view,settings));
    settings.exposure=0; CHECK(validate_render_settings(f.view,settings));
    settings.exposure=-1; CHECK_FALSE(validate_render_settings(f.view,settings));
    settings.exposure=std::numeric_limits<float>::infinity(); CHECK_FALSE(validate_render_settings(f.view,settings));
    settings.exposure=1; settings.clear_rgb.x()=-0.1f; CHECK_FALSE(validate_render_settings(f.view,settings));
    settings.clear_rgb.x()=std::numeric_limits<float>::quiet_NaN(); CHECK_FALSE(validate_render_settings(f.view,settings));
    settings.clear_rgb.x()=8; CHECK(validate_render_settings(f.view,settings));
    auto huge=RenderView::create(f.scene,{UINT32_MAX,UINT32_MAX}).value(); CHECK_FALSE(validate_render_settings(huge,settings));
    auto moved=std::move(f.view); CHECK_FALSE(validate_render_settings(f.view,settings)); CHECK(validate_render_settings(moved,settings));
}
TEST_CASE("pipeline push transform rows preserve affine camera composition and reject float overflow") {
    Fixture f; Trsd camera; camera.translation={1,2,3};
    ViewDescription desc{64,64}; desc.camera_world=Transformd::from_trs(camera).value(); desc.projection(0,0)=2;
    auto view=RenderView::create(f.scene,desc).value();
    Trsd model; model.translation={3,4,5}; model.scale={2,3,-1};
    auto rows=dk::render::detail::clip_rows(view,Transformd::from_trs(model).value()).value();
    CHECK(rows[0]==4); CHECK(rows[3]==4); CHECK(rows[5]==3); CHECK(rows[7]==2); CHECK(rows[10]==-1); CHECK(rows[11]==2); CHECK(rows[15]==1);
    model.translation.x()=std::numeric_limits<double>::max()/2;
    CHECK_FALSE(dk::render::detail::clip_rows(view,Transformd::from_trs(model).value()));
}
TEST_CASE("empty render frame exposes no owners or pixels") {
    RenderFrame frame; CHECK_FALSE(frame); CHECK(frame.info().scene.is_nil()); CHECK(frame.info().draw_count==0);
    CHECK(frame.plan_text().empty()); CHECK_FALSE(frame.color()); CHECK_FALSE(frame.read_rgba8({}));
}
TEST_CASE("orthographic basic pipeline accepts matrices while atmosphere remains unsupported") {
    Fixture f; ViewDescription d{128,64}; d.projection=orthographic_projection({3,.1,100},2).value();
    auto view=RenderView::create(f.scene,d); REQUIRE(view);
    CHECK(validate_render_settings(*view,unlit_preview_settings()));
    CHECK_FALSE(dk::render::detail::perspective_depth(d));
    const auto rows=dk::render::detail::clip_rows(*view,Transformd{}); REQUIRE(rows);
    CHECK((*rows)[15]==1); CHECK((*rows)[14]==0);
}
TEST_CASE("Lambert normal matrix preserves tangent orthogonality under shear and reflection") {
    Mat4d m=Mat4d::Identity(); m.topLeftCorner<3,3>() << -2,1,.3, 0,3,.5, 0,0,.7;
    const auto transform=Transformd::from_matrix(m).value();
    const auto normal=dk::render::detail::normal_matrix(transform).value();
    const Vec3d tangent{1,2,3},n=Vec3d{-2,1,0}.normalized();
    CHECK(std::abs((normal*n).dot(m.topLeftCorner<3,3>()*tangent))<1e-12);
    CHECK((normal*Vec3d::UnitX()).x()<0);
    m(2,2)=0; CHECK_FALSE(dk::render::detail::normal_matrix(Transformd::from_matrix(m).value()));
    m=Mat4d::Identity(); m.topLeftCorner<3,3>()*=1e-100;
    auto tiny=dk::render::detail::normal_matrix(Transformd::from_matrix(m).value());
    REQUIRE(tiny); CHECK(tiny->allFinite()); CHECK((*tiny- Mat3d::Identity()).norm()<1e-12);
}

TEST_CASE("atmosphere perspective reconstructs physical distances and rejects ambiguous projections") {
    ViewDescription v{32,32}; v.projection=Mat4d::Zero();
    const double n=10,f=50000; auto& p=v.projection;
    p(0,0)=1.2; p(1,1)=-1.5; p(0,2)=.2; p(1,2)=-.1; p(2,2)=f/(n-f); p(2,3)=n*f/(n-f); p(3,2)=-1;
    auto result=dk::render::detail::perspective_depth(v); REQUIRE(result);
    CHECK(std::abs(result->near_m-n)<1e-9); CHECK(std::abs(result->far_m-f)<1e-5);
    for(double distance : {10.,100.,12000.,50000.}) {
        Vec4d point{distance*.3,distance*.2,-distance,1}; Vec4d clip=p*point;
        const double depth=clip.z()/clip.w(),restored=p(2,3)/(depth+p(2,2));
        CHECK(std::abs(restored-distance)<1e-5);
        Vec3d ray{(clip.x()/clip.w()+p(0,2))/p(0,0),(clip.y()/clip.w()+p(1,2))/p(1,1),-1};
        CHECK(std::abs(restored*ray.norm()-point.head<3>().norm())<1e-5);
    }
    auto bad=v; bad.projection=Mat4d::Identity(); CHECK_FALSE(dk::render::detail::perspective_depth(bad));
    bad=v; bad.projection(2,2)=-1; CHECK_FALSE(dk::render::detail::perspective_depth(bad));
    bad=v; bad.projection(2,2)=n/(f-n); bad.projection(2,3)=n*f/(f-n); CHECK_FALSE(dk::render::detail::perspective_depth(bad));
    bad=v; bad.projection(0,1)=.1; CHECK_FALSE(dk::render::detail::perspective_depth(bad));
    bad=v; Trsd scale; scale.scale.x()=2; bad.camera_world=Transformd::from_trs(scale).value(); CHECK_FALSE(dk::render::detail::perspective_depth(bad));
    Fixture fixture; RenderSettings settings; settings.linear_readback=true;
    CHECK(validate_render_settings(fixture.view,settings));
    CHECK_FALSE(validate_render_settings(RenderView::create(fixture.scene,{257,256}).value(),settings));
    CHECK_FALSE(RenderFrame{}.read_linear_rgba32f({}));
}

TEST_CASE("atmosphere perspective accepts inclusive boundaries with reconstruction rounding") {
    for(const auto limits : {std::pair{1.,10000.},std::pair{100.,1000000.}}) {
        ViewDescription v{32,32}; v.projection=Mat4d::Zero();
        auto& p=v.projection; p(0,0)=1.2; p(1,1)=-1.5; p(3,2)=-1;
        const auto set=[&](double n,double f) { p(2,2)=f/(n-f); p(2,3)=n*f/(n-f); };
        set(limits.first,limits.second); REQUIRE(dk::render::detail::perspective_depth(v));
        set(limits.first,limits.second*1.000001); CHECK_FALSE(dk::render::detail::perspective_depth(v));
    }
}

TEST_CASE("environment render settings reject ambiguous modes time and fixed shadow regions") {
    EnvironmentRenderSettings s; REQUIRE(validate_environment_render_settings(s));
    auto bad=s; bad.mode=static_cast<CloudRenderMode>(2); CHECK_FALSE(validate_environment_render_settings(bad));
    bad=s; bad.time_seconds=std::numeric_limits<double>::quiet_NaN(); CHECK_FALSE(validate_environment_render_settings(bad));
    bad=s; bad.time_seconds=1e10; CHECK_FALSE(validate_environment_render_settings(bad));
    bad=s; bad.view_id=0; CHECK_FALSE(validate_environment_render_settings(bad));
    bad=s; bad.shadow_resolution=17; CHECK_FALSE(validate_environment_render_settings(bad));
    bad=s; bad.shadow_extent_m=0; CHECK_FALSE(validate_environment_render_settings(bad));
    bad=s; bad.shadow_center_m.x()=1e10; CHECK_FALSE(validate_environment_render_settings(bad));
    s.mode=CloudRenderMode::temporal; s.time_seconds=-1e9; REQUIRE(validate_environment_render_settings(s));
}
