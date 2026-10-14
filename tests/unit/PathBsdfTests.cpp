#include "../RenderAssetSupport.hpp"
#include "../PathBsdfOracle.hpp"
#include "../../engine/render/pathtracing/src/PathBsdf.hpp"
#include <dk/scene/SceneDocument.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <iostream>

using namespace dk;
using namespace dk::render;
using namespace dk::render::detail;
namespace {
const Vec3d n{0,0,1};
PathBsdf bsdf(double m,double r,Vec3d c=Vec3d{.7,.4,.2}) { return {c,m,r,PathTraceMaterialModel::metallic_roughness}; }
struct Stats { Vec3d mean=Vec3d::Zero(),se=Vec3d::Zero(); double accepted=0; };
Stats samples(const PathBsdf& b,const Vec3d& wo,std::uint32_t count) {
    Stats s; Vec3d square=Vec3d::Zero();
    for(std::uint32_t i=0;i<count;++i) {
        const auto p=sample_path_bsdf(b,n,wo,path_trace_sample(73,0,i,0,6),path_trace_sample(73,0,i,0,7));
        if(p.evaluated.pdf>0) {
            const Vec3d value=p.evaluated.value*(p.direction.z()/p.evaluated.pdf);
            s.mean+=value; square+=value.cwiseProduct(value); s.accepted+=1;
        }
    }
    s.mean/=count; s.se=((square/count-s.mean.cwiseProduct(s.mean)).cwiseMax(0)/(count-1)).cwiseSqrt(); s.accepted/=count; return s;
}
}
TEST_CASE("GGX CPU analytic endpoints reciprocity and direction domains") {
    for(double m:{0.,.5,1.}) for(double r:{0.,.05,.2,.5,1.}) {
        const auto b=bsdf(m,r); const double a=std::max(.0025,r*r);
        const Vec3d f0=Vec3d::Constant(.04)*(1-m)+b.base_color*m;
        const Vec3d expected=b.base_color*((1-m)*.96*.96/path_oracle::pi)+f0/(4*path_oracle::pi*a*a);
        const auto evaluated=eval_path_bsdf(b,n,n,n);
        REQUIRE((evaluated.value-expected).norm()<1e-10*expected.norm());
        for(double cosine:{1.,.5,.01,1e-6}) {
            const Vec3d wo{std::sqrt(1-cosine*cosine),0,cosine},wi=Vec3d{-.4,.2,.7}.normalized();
            const auto x=eval_path_bsdf(b,n,wo,wi),y=eval_path_bsdf(b,n,wi,wo);
            REQUIRE(x.value.allFinite()); REQUIRE(x.pdf>0); REQUIRE((x.value-y.value).norm()<1e-10);
            REQUIRE((x.value-path_oracle::evaluate(b.base_color,m,r,wo,wi)).norm()<1e-10);
        }
        REQUIRE(eval_path_bsdf(b,n,n,-n).pdf==0); REQUIRE(eval_path_bsdf(b,n,Vec3d::UnitX(),n).pdf==0);
        REQUIRE(sample_path_bsdf(b,n,-n,.2,.3).evaluated.pdf==0);
        REQUIRE(sample_path_bsdf(b,n,n,1,.3).evaluated.pdf==0);
    }
    REQUIRE(eval_path_bsdf(bsdf(0,1,Vec3d::Zero()),n,n,n).value.minCoeff()>0);
    REQUIRE((eval_path_bsdf(bsdf(1,0),n,n,n).value-eval_path_bsdf(bsdf(1,.05),n,n,n).value).norm()<1e-8);
}
TEST_CASE("GGX CPU independent furnace energy and visible sampling mean") {
    for(double m:{0.,.5,1.}) for(double r:{.2,.5,1.}) for(double cosine:{1.,.1}) {
        const auto b=bsdf(m,r,Vec3d::Ones()); const Vec3d wo{std::sqrt(1-cosine*cosine),0,cosine};
        const Vec3d low=path_oracle::furnace(b.base_color,m,r,wo,192),expected=path_oracle::furnace(b.base_color,m,r,wo,384);
        INFO("m="<<m<<" r="<<r<<" cosine="<<cosine<<" oracle="<<expected.transpose());
        REQUIRE((expected-low).cwiseAbs().maxCoeff()<.005);
        REQUIRE(expected.minCoeff()>0); REQUIRE(expected.maxCoeff()<=1.005);
        const auto sample=samples(b,wo,262144);
        REQUIRE((sample.mean-expected).cwiseAbs().maxCoeff()<.02*expected.maxCoeff());
        REQUIRE(((sample.mean-expected).cwiseAbs().array()<=5*sample.se.array()+.005).all());
    }
    const auto rough=samples(bsdf(1,1,Vec3d::Ones()),n,262144);
    REQUIRE(rough.mean.x()==Catch::Approx(1-std::log(2.)).margin(.002));
    const auto smooth=samples(bsdf(1,0,Vec3d::Ones()),n,262144);
    REQUIRE(smooth.mean.x()==Catch::Approx(1).margin(.001));
    std::cout<<"GGX rough white analytic="<<1-std::log(2.)<<" sampled="<<rough.mean.x()<<" smooth="<<smooth.mean.x()<<'\n';
}
TEST_CASE("GGX CPU PDF mass matches rejected samples and rotated normals") {
    const Vec3d wo=Vec3d{.8,0,.6}; const auto b=bsdf(.5,.7); double mass=0;
    constexpr int size=512;
    for(int i=0;i<size;++i) for(int j=0;j<size;++j) {
        const double z=(i+.5)/size,phi=2*path_oracle::pi*(j+.5)/size,r=std::sqrt(1-z*z);
        mass+=eval_path_bsdf(b,n,wo,{r*std::cos(phi),r*std::sin(phi),z}).pdf;
    }
    mass*=2*path_oracle::pi/(size*size);
    const auto sample=samples(b,wo,262144); REQUIRE(mass==Catch::Approx(sample.accepted).margin(.003)); REQUIRE(mass<.95);
    const Mat3d rotation=Eigen::AngleAxisd(.8,Vec3d{1,2,3}.normalized()).toRotationMatrix();
    const Vec3d wi=Vec3d{.1,-.4,.7}.normalized();
    REQUIRE((eval_path_bsdf(b,n,wo,wi).value-eval_path_bsdf(b,rotation*n,rotation*wo,rotation*wi).value).norm()<1e-12);
    const auto selected=sample_path_bsdf(b,rotation*n,rotation*wo,.17,.31);
    REQUIRE(selected.evaluated.pdf==Catch::Approx(eval_path_bsdf(b,rotation*n,rotation*wo,selected.direction).pdf));
}
TEST_CASE("GGX CPU explicit scene model freezes metallic roughness and default material") {
    RenderMemory memory; auto document=SceneDocument::create().value(); CpuAsset asset; asset.mesh.id=AssetId::generate().value();
    const auto entity=document->create_entity().value(); document->set_asset_references(entity,{{asset.mesh.id,AssetKind::mesh}}).value();
    MeshPrimitive p; p.positions={{-5,-5,0},{5,-5,0},{0,5,0}}; p.indices={0,1,2}; asset.mesh.primitives.push_back(p);
    const auto source=RenderScene::extract(memory.heap,document->snapshot().value()).value(); const std::array assets{&asset};
    REQUIRE_FALSE(PathTraceScene::create(memory.heap,source,assets));
    const auto model=PathTraceMaterialModel::metallic_roughness; auto scene=PathTraceScene::create(memory.heap,source,assets,{},model); REQUIRE(scene);
    REQUIRE(scene->material_model()==model); const auto old=scene->fingerprint();
    PathTraceSettings s; s.max_bounces=1; s.rr_start=0; s.environment_rgb=Vec3d::Ones(); s.estimator=PathTraceEstimator::bsdf_only;
    const PathTraceRay ray{{0,0,1},{0,0,-1}}; const auto value=estimate_path_reference(*scene,ray,s,{0,65536}); REQUIRE(value);
    REQUIRE(value->mean.x()==Catch::Approx(1-std::log(2.)).margin(.004));
    MaterialData material; material.id=AssetId::generate().value(); asset.materials.push_back(material); asset.mesh.primitives[0].material=material.id;
    asset.materials[0].roughness=.5f; auto changed=PathTraceScene::create(memory.heap,source,assets,{},model); REQUIRE(changed); REQUIRE(changed->fingerprint()!=old);
    asset.materials[0].metallic=.5f; auto mixed=PathTraceScene::create(memory.heap,source,assets,{},model); REQUIRE(mixed); REQUIRE(mixed->fingerprint()!=changed->fingerprint());
    for(float invalid:{-1.f,2.f,std::numeric_limits<float>::quiet_NaN()}) {
        asset.materials[0].metallic=invalid; REQUIRE_FALSE(PathTraceScene::create(memory.heap,source,assets,{},model));
        asset.materials[0].metallic=0; asset.materials[0].roughness=invalid; REQUIRE_FALSE(PathTraceScene::create(memory.heap,source,assets,{},model)); asset.materials[0].roughness=1;
    }
    REQUIRE_FALSE(PathTraceScene::create(memory.heap,source,assets,{},static_cast<PathTraceMaterialModel>(99)));
    REQUIRE(scene->fingerprint()==old); REQUIRE(estimate_path_reference(*scene,ray,s,{0,65536})->mean==value->mean);
    s.model_version=2; REQUIRE_FALSE(estimate_path_reference(*scene,ray,s));
}
