#include "../RenderAssetSupport.hpp"
#include <dk/render/PathTraceReference.hpp>
#include <dk/scene/SceneDocument.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

using namespace dk;
using namespace dk::render;
namespace {
struct Fixture : RenderMemory {
    std::unique_ptr<SceneDocument> doc=SceneDocument::create().value();
    CpuAsset asset=render_asset();
    EntityId entity=doc->create_entity().value();
    Fixture() {
        asset.mesh.primitives.resize(1); asset.materials.resize(1); asset.textures.resize(1);
        auto& p=asset.mesh.primitives[0]; p.positions={{-2,-2,0},{2,-2,0},{0,2,0}}; p.texcoords={{0,0},{0,0},{0,0}};
        asset.materials[0].metallic=0;
        auto& t=asset.textures[0]; t.sampler={}; t.sampler.mag_filter=TextureFilter::nearest; t.origin="fixture/base.png";
        t.rgba8={std::byte{128},std::byte{0},std::byte{0},std::byte{0}, std::byte{0},std::byte{255},std::byte{0},std::byte{255},
            std::byte{0},std::byte{0},std::byte{255},std::byte{128}, std::byte{255},std::byte{255},std::byte{255},std::byte{255}};
        doc->set_asset_references(entity,{{asset.mesh.id,AssetKind::mesh}}).value();
    }
    void uv(double u,double v) { for(auto& t:asset.mesh.primitives[0].texcoords) t={float(u),float(v)}; }
    Result<PathTraceScene> scene(PathTraceSceneLimits limits={}) {
        const auto source=RenderScene::extract(heap,doc->snapshot().value()).value(); const std::array assets{static_cast<const CpuAsset*>(&asset)};
        return PathTraceScene::create(heap,source,assets,limits);
    }
    Vec3d color() {
        PathTraceSettings s; s.max_bounces=1; s.rr_start=0; s.estimator=PathTraceEstimator::bsdf_only; s.environment_rgb=Vec3d::Ones();
        return estimate_path_reference(scene().value(),{{0,0,1},{0,0,-1}},s,{0,1}).value().mean;
    }
};
const PathTraceRay down{{0,0,1},{0,0,-1}};
}
TEST_CASE("texture CPU sRGB filtering UV and all wrap modes have independent expected values") {
    Fixture f; f.uv(.25,.25); const double red=.21586050011389926;
    CHECK((f.color()-Vec3d{red,0,0}).norm()<1e-12);
    f.uv(.75,.25); CHECK(f.color()==Vec3d{0,1,0}); f.uv(.25,.75); CHECK(f.color()==Vec3d{0,0,1});
    f.uv(-.25,1.25); CHECK(f.color()==Vec3d{0,1,0});
    auto& t=f.asset.textures[0]; t.sampler.wrap_s=TextureWrap::clamp; t.sampler.wrap_t=TextureWrap::clamp;
    CHECK(f.color()==Vec3d{0,0,1}); t.sampler.wrap_s=TextureWrap::mirrored_repeat; t.sampler.wrap_t=TextureWrap::mirrored_repeat;
    CHECK(f.color()==Vec3d{0,0,1}); f.uv(1.25,-.25); CHECK(f.color()==Vec3d{0,1,0});
    t.sampler.mag_filter=TextureFilter::linear; f.uv(.5,.5);
    CHECK((f.color()-Vec3d{(red+1)/4,.5,.5}).norm()<1e-12);
    t.sampler.min_filter=TextureFilter::nearest_mipmap_nearest;
    CHECK((f.color()-Vec3d{(red+1)/4,.5,.5}).norm()<1e-12);
    t.sampler.wrap_s=TextureWrap::repeat; f.uv(0,.25); CHECK((f.color()-Vec3d{red*.5,.5,0}).norm()<1e-12);
}
TEST_CASE("texture CPU mask alpha is linear opaque ignores it and threshold equality survives") {
    Fixture f; auto& m=f.asset.materials[0]; f.uv(.25,.25);
    CHECK(f.scene()->nearest(down)->has_value()); m.alpha_mode=AlphaMode::mask;
    CHECK_FALSE(f.scene()->nearest(down)->has_value()); f.uv(.25,.75); CHECK(f.scene()->nearest(down)->has_value());
    m.base_color.w()=.5f; CHECK_FALSE(f.scene()->nearest(down)->has_value());
    f.uv(.75,.75); CHECK(f.scene()->nearest(down)->has_value());
    m.emissive=Vec3f::Ones(); CHECK(f.scene()->masked_emitter_count()==1);
    m.alpha_mode=AlphaMode::opaque; CHECK(f.scene()->masked_emitter_count()==0);
    m.alpha_mode=AlphaMode::mask; m.base_color_texture.reset(); m.base_color.w()=.5f;
    CHECK(f.scene()->nearest(down)->has_value()); m.base_color.w()=.49f; CHECK_FALSE(f.scene()->nearest(down)->has_value());
}
TEST_CASE("texture CPU immutable ownership provenance and fingerprint survive source edits") {
    Fixture f; f.uv(.75,.75); const auto old=f.scene().value(); const auto info=old.texture_info(0).value();
    CHECK(old.texture_count()==1); CHECK(info.mesh==f.asset.mesh.id); CHECK(info.texture==f.asset.textures[0].id);
    CHECK(info.origin=="fixture/base.png"); CHECK_FALSE(old.texture_info(1));
    auto fingerprint=old.fingerprint(); f.asset.textures[0].rgba8[0]=std::byte{0}; CHECK(f.scene()->fingerprint()!=fingerprint);
    fingerprint=f.scene()->fingerprint(); f.asset.textures[0].sampler.wrap_s=TextureWrap::clamp; CHECK(f.scene()->fingerprint()!=fingerprint);
    fingerprint=f.scene()->fingerprint(); f.uv(.25,.25); CHECK(f.scene()->fingerprint()!=fingerprint);
    f.asset={}; CHECK(old.texture_info(0)->origin=="fixture/base.png"); CHECK(old.nearest(down)->has_value());
}
TEST_CASE("texture CPU rejects malformed data and enforces count and byte capacities") {
    Fixture f; const auto old=f.scene().value(); auto& t=f.asset.textures[0];
    t.rgba8.pop_back(); CHECK_FALSE(f.scene()); t.rgba8.push_back(std::byte{255});
    t.sampler.mag_filter=TextureFilter::linear_mipmap_linear; CHECK_FALSE(f.scene()); t.sampler.mag_filter=TextureFilter::nearest;
    t.sampler.wrap_s=static_cast<TextureWrap>(1); CHECK_FALSE(f.scene()); t.sampler.wrap_s=TextureWrap::repeat;
    f.asset.materials[0].alpha_cutoff=std::numeric_limits<float>::quiet_NaN(); CHECK_FALSE(f.scene()); f.asset.materials[0].alpha_cutoff=.5;
    f.asset.mesh.primitives[0].texcoords[0].x()=std::numeric_limits<float>::infinity(); CHECK_FALSE(f.scene()); f.uv(0,0);
    CHECK_FALSE(f.scene({4096,4096,0})); CHECK_FALSE(f.scene({4096,4096,64,15})); CHECK(f.scene({4096,4096,64,16}));
    const auto primitive=f.asset.mesh.primitives[0]; const auto material=f.asset.materials[0]; const auto texture=t;
    for(int i=1;i<65;++i) {
        auto p=primitive; auto m=material; auto image=texture;
        image.id=AssetId::generate().value(); m.id=AssetId::generate().value(); m.base_color_texture=image.id; p.material=m.id;
        f.asset.textures.push_back(std::move(image)); f.asset.materials.push_back(m); f.asset.mesh.primitives.push_back(std::move(p));
        if(i==63) CHECK(f.scene()->texture_count()==64);
    }
    CHECK_FALSE(f.scene()); CHECK(f.scene(desktop_path_scene_limits())->texture_count()==65); CHECK(old.texture_count()==1); CHECK(old.nearest(down)->has_value());
}

TEST_CASE("texture CPU allocation failure unwinds copied bytes and preserves old scene") {
    Fixture f; f.uv(.75,.75); const auto old=f.scene().value();
    const auto bounded=f.system.create_heap({"texture-limit",memory::DomainCategory::render,65536}).value();
    const auto source=RenderScene::extract(f.heap,f.doc->snapshot().value()).value();
    const std::array assets{static_cast<const CpuAsset*>(&f.asset)};
    auto& t=f.asset.textures[0]; t.width=t.height=512; t.rgba8.resize(512*512*4,std::byte{255});
    CHECK_FALSE(PathTraceScene::create(bounded,source,assets)); CHECK(bounded.snapshot().live_allocations==0);
    CHECK(old.nearest(down)->has_value()); CHECK(old.texture_info(0)->width==2);
    t.width=t.height=2; t.rgba8.resize(16);
    { const auto recovered=PathTraceScene::create(bounded,source,assets); REQUIRE(recovered); CHECK(recovered->texture_count()==1); }
    CHECK(bounded.snapshot().live_allocations==0); bounded.begin_close();
}

TEST_CASE("texture CPU provenance ignores transient import UUIDs but preserves binding content") {
    Fixture f; const auto original=f.scene().value();
    auto id=AssetId::generate().value(); f.asset.textures[0].id=id; f.asset.materials[0].base_color_texture=id;
    CHECK(f.scene()->fingerprint()==original.fingerprint());
    f.asset.textures[0].rgba8[0]=std::byte{0};
    CHECK(f.scene()->fingerprint()!=original.fingerprint());
}
