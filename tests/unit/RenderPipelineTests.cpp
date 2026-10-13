#include "PipelinePolicy.hpp"
#include <dk/scene/SceneDocument.hpp>
#include <dk/memory/MemorySystem.hpp>
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
