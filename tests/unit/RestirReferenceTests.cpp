#include "../RestirTestSupport.hpp"
#include "../PathBsdfOracle.hpp"
#include "../../engine/render/pathtracing/src/RestirReferenceInternal.hpp"
#include <dk/math/Projection.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <iostream>
#include <thread>
#include <chrono>

using namespace restir_test;
namespace {
struct Population { Vec3d mean=Vec3d::Zero(),se=Vec3d::Zero(); double mse=0; };
Population population(const PathTraceScene& scene,PathTraceSettings s,RestirDiSettings ris,std::uint32_t count,
    const Vec3d& expected,bool classic=false,const PathTraceRay& ray=down) {
    std::array<Vec3d,8> means; Population p;
    for(std::uint32_t i=0;i<8;++i) {
        s.seed=101+7919*i;
        if(classic) { auto r=estimate_path_reference(scene,ray,s,{0,count}); REQUIRE(r); means[i]=r->mean; }
        else { auto r=estimate_restir_reference(scene,ray,s,ris,{0,count}); REQUIRE(r); means[i]=r->mean; }
        p.mean+=means[i]/8; p.mse+=(means[i]-expected).squaredNorm()/24;
    }
    for(const auto& mean:means) p.se+=(mean-p.mean).cwiseAbs2()/56;
    p.se=p.se.cwiseSqrt(); return p;
}
void check_mean(const Population& actual,const Vec3d& expected,const Vec3d& reference_se=Vec3d::Zero()) {
    const Vec3d error=(actual.mean-expected).cwiseAbs(),combined=(actual.se.cwiseAbs2()+reference_se.cwiseAbs2()).cwiseSqrt();
    INFO("mean="<<actual.mean.transpose()<<" expected="<<expected.transpose()<<" se="<<combined.transpose());
    for(int k=0;k<3;++k) {
        CHECK(error[k]<=.02*std::abs(expected[k])+1e-4);
        CHECK(error[k]<=5*combined[k]+1e-4);
    }
}
Vec3d area_reference(const Vec3d& color,double metal,double rough,const Vec3d& wo,bool ggx,int size) {
    if(ggx) return 2*path_oracle::area(color,metal,rough,wo,size);
    double sum=0,dx=2.0/size;
    for(int y=0;y<size;++y) for(int x=0;x<size;++x) {
        const double a=-1+(x+.5)*dx,b=-1+(y+.5)*dx,r2=a*a+b*b+4;
        sum+=4/(r2*r2)*dx*dx;
    }
    return color*(2*sum/path_oracle::pi);
}
}

TEST_CASE("RESTIR reservoir enumerated expectation and selection frequencies") {
    const std::array q{.2,.3,.5},target{0.,2.,8.},f{0.,3.,5.}; double expected=0;
    for(unsigned a=0;a<3;++a) for(unsigned b=0;b<3;++b) {
        const double wa=target[a]/q[a],wb=target[b]/q[b],sum=wa+wb;
        if(sum==0) continue;
        for(bool second:{false,true}) {
            const double probability=second ? wb/sum : wa/sum; if(probability==0) continue;
            RisReservoir r; REQUIRE(r.stream(target[a],q[a],.5));
            REQUIRE(r.stream(target[b],q[b],second ? 0 : std::nextafter(1.,0.)));
            const auto index=r.selected()==0 ? a : b;
            expected+=q[a]*q[b]*probability*f[index]*r.normalization();
        }
    }
    CHECK(expected==Catch::Approx(8).margin(1e-12));
    unsigned selected=0;
    for(unsigned i=0;i<65536;++i) {
        RisReservoir r; REQUIRE(r.stream(1,1,.5));
        REQUIRE(r.stream(3,1,restir_reference_sample(7,0,i,1,RestirRandomPurpose::replacement)));
        selected+=r.selected()==1;
    }
    CHECK(double(selected)/65536==Catch::Approx(.75).margin(.006));
}
TEST_CASE("RESTIR reservoir M one zero draws and extreme weights") {
    RisReservoir empty; CHECK(empty.normalization()==0); CHECK(empty.selected()==RisReservoir::invalid_sample);
    REQUIRE(empty.stream(0,.25,.7)); CHECK(empty.candidates()==1); CHECK(empty.normalization()==0);
    REQUIRE(empty.stream(4,.5,.7)); CHECK(empty.candidates()==2); CHECK(empty.normalization()==1);
    REQUIRE(empty.stream(0,.25,.7)); CHECK(empty.selected()==1); CHECK(empty.normalization()==Catch::Approx(2./3));
    for(auto [target,pdf]:{std::pair{7.,.25},std::pair{1e200,1e-50},std::pair{1e-200,1e50}}) {
        RisReservoir r; REQUIRE(r.stream(target,pdf,.9)); CHECK(r.normalization()==Catch::Approx(1/pdf));
    }
}
TEST_CASE("RESTIR counter domain is frozen distinct and batch independent") {
    const std::array<std::uint32_t,5> golden{1082515,529820,8300935,3198819,12519215};
    for(std::uint32_t d=0;d<5;++d) {
        const auto purpose=static_cast<RestirRandomPurpose>(d);
        CHECK(restir_reference_sample(1,17,29,3,purpose)==(double(golden[d])+.5)/16777216.0);
        CHECK(restir_reference_sample(1,17,29,3,purpose)!=path_trace_sample(1,17,29,3,d));
        CHECK(restir_reference_sample(1,17,29,3,purpose)!=restir_reference_sample(1ull<<32,17,29,3,purpose));
    }
    double sum=0;
    for(std::uint32_t i=0;i<16384;++i) {
        const auto u=restir_reference_sample(101,3,i,7,RestirRandomPurpose::replacement);
        REQUIRE(u>0); REQUIRE(u<1); sum+=u;
    }
    CHECK(sum/16384==Catch::Approx(.5).margin(.01));
}
TEST_CASE("RESTIR reservoir rejects invalid draws without changing selection") {
    RisReservoir r; REQUIRE(r.stream(2,.25,.5));
    for(const auto v:{Vec3d{-1,1,.5},Vec3d{1,0,.5},Vec3d{1,1,1},Vec3d{1,1,-.1},
        Vec3d{NAN,1,.5},Vec3d{1,INFINITY,.5},Vec3d{1,1,NAN},Vec3d{1e308,.01,.5},Vec3d{1e-300,1e300,.5}}) {
        CHECK_FALSE(r.stream(v.x(),v.y(),v.z())); CHECK(r.candidates()==1); CHECK(r.selected()==0);
        CHECK(r.weight_sum()==8); CHECK(r.target()==2); CHECK(r.normalization()==4);
    }
}
TEST_CASE("RESTIR merge corrects disjoint support and retains zero source counts") {
    const std::array<RisReuseSource,2> disjoint{{{1,1,1},{1,1,1}}};
    const std::array<std::uint8_t,4> separate{1,0,0,1}; double expected=0;
    for(double u:{0.,.9}) {
        const auto r=resample_ris_sources(disjoint,separate,std::array{.5,u}); REQUIRE(r);
        expected+=.5*(r->selected==0 ? 2 : 6)*r->normalization;
    }
    CHECK(expected==Catch::Approx(8)); // A naive total-M normalization would produce 4.
    // Source A: target support {0}, proposal uniform over {0,1}; B: full support.
    double total=0;
    for(unsigned a=0;a<2;++a) for(unsigned b=0;b<2;++b) {
        const std::array<RisReuseSource,2> sources{{{a==0 ? 1. : 0.,a==0 ? 2. : 0.,1},{1,2,1}}};
        const std::array<std::uint8_t,4> support{std::uint8_t(a==0),std::uint8_t(b==0),1,1};
        const double probability_b=a==0 ? .5 : 1;
        for(bool take_b:{false,true}) {
            const double probability=take_b ? probability_b : 1-probability_b; if(probability==0) continue;
            const auto r=resample_ris_sources(sources,support,std::array{.5,take_b ? 0. : .99}); REQUIRE(r);
            const unsigned light=r->selected==0 ? a : b;
            total+=.25*probability*(light==0 ? 2 : 6)*r->normalization;
        }
    }
    CHECK(total==Catch::Approx(8).margin(1e-12));
}
TEST_CASE("RESTIR merge overlap unequal effective M and correlated sources preserve expectation") {
    for(std::uint32_t m:{1u,7u,32u}) {
        double expected=0;
        for(unsigned a=0;a<2;++a) for(unsigned b=0;b<2;++b) {
            // A samples {0,1}, B samples {1,2}, both uniformly.
            const std::array<RisReuseSource,2> sources{{{1,2,m},{1,2,2}}};
            const std::array<std::uint8_t,4> support{1,std::uint8_t(b==0),std::uint8_t(a==1),1};
            for(bool second:{false,true}) {
                const double probability=second ? 2./(m+2) : double(m)/(m+2);
                const auto r=resample_ris_sources(sources,support,std::array{.5,second ? 0. : .999}); REQUIRE(r);
                const unsigned light=r->selected==0 ? a : b+1;
                expected+=.25*probability*std::array{1.,4.,9.}[light]*r->normalization;
            }
        }
        CHECK(expected==Catch::Approx(14).margin(1e-12));
    }
    double expected=0;
    for(unsigned sample=0;sample<2;++sample) {
        const std::array<RisReuseSource,2> correlated{{{1,2,1},{1,2,5}}};
        const auto r=resample_ris_sources(correlated,std::array<std::uint8_t,4>{1,1,1,1},std::array{.2,.3}); REQUIRE(r);
        expected+=.5*std::array{2.,6.}[sample]*r->normalization;
    }
    CHECK(expected==Catch::Approx(8));
}
TEST_CASE("RESTIR merge validates support empty reservoirs and capacity") {
    CHECK(resample_ris_sources({}, {}, {}));
    std::array<RisReuseSource,2> sources{{{0,0,4},{0,0,2}}};
    const auto empty=resample_ris_sources(sources,std::array<std::uint8_t,4>{0,0,0,0},std::array{.2,.3}); REQUIRE(empty);
    CHECK(empty->candidates==6); CHECK(empty->normalization==0); CHECK(empty->selected==RisReservoir::invalid_sample);
    CHECK_FALSE(resample_ris_sources(sources,{},std::array{.2,.3}));
    sources[0]={1,2,1}; CHECK_FALSE(resample_ris_sources(sources,std::array<std::uint8_t,4>{0,0,0,0},std::array{.2,.3}));
    sources[0]={1,INFINITY,1}; CHECK_FALSE(resample_ris_sources(sources,std::array<std::uint8_t,4>{1,1,1,1},std::array{.2,.3}));
    sources[0]={1,2,1u<<25}; CHECK_FALSE(resample_ris_sources(sources,std::array<std::uint8_t,4>{1,1,1,1},std::array{.2,.3}));
}
TEST_CASE("RESTIR light measures targets identities and mask domain are explicit") {
    using namespace dk::render::detail;
    Fixture f; f.quad(-8,8,0,{.6f,.6f,.6f}); f.quad(-1,1,2,{0,0,0},{2,2,2},true);
    const auto scene=f.scene(); const auto& state=*PathReferenceAccess::get(scene); auto s=settings(); s.environment_rgb=Vec3d::Ones();
    const auto hit=scene.nearest(down).value().value(); const PathBsdf bsdf{Vec3d::Constant(double(.6f)),0,1,PathTraceMaterialModel::lambert};
    const auto area=sample_restir_light(state,s,{.75,.2,.4,.3}); CHECK(area.kind==RestirLightKind::triangle);
    CHECK(area.proposal_pdf==Catch::Approx(.125)); // half class * half triangle / area 2
    const auto eval=evaluate_restir_light(state,s,hit,bsdf,{0,0,1},area); REQUIRE(eval);
    const double d2=eval->point.squaredNorm();
    CHECK(eval->contribution.x()==Catch::Approx(double(.6f)*2*4/(path_oracle::pi*d2*d2)));
    const auto env=sample_restir_light(state,s,{.25,.2,.1,.3}); CHECK(env.kind==RestirLightKind::environment);
    CHECK(env.proposal_pdf==Catch::Approx(.5/(4*path_oracle::pi)));
    CHECK(evaluate_restir_light(state,s,hit,bsdf,{0,0,1},env)->target>0);
    auto wrong=area; ++wrong.scene_fingerprint; CHECK_FALSE(evaluate_restir_light(state,s,hit,bsdf,{0,0,1},wrong));
    s.environment_rgb*=2; CHECK_FALSE(evaluate_restir_light(state,s,hit,bsdf,{0,0,1},area));
    for(std::size_t i=2;i<f.asset.materials.size();++i) f.asset.materials[i].alpha_mode=AlphaMode::mask;
    const auto masked=f.scene(); s.environment_rgb.setZero();
    CHECK(sample_restir_light(*PathReferenceAccess::get(masked),s,{.2,.2,.2,.2}).kind==RestirLightKind::none);
}
TEST_CASE("RESTIR CPU area mean convergence and initial sample partition") {
    Fixture f; f.quad(-8,8,0,{.6f,.6f,.6f}); f.quad(-1,1,2,{0,0,0},{2,2,2},true);
    const auto scene=f.scene(); const auto s=settings(); RestirDiSettings r;
    const Vec3d expected=area_reference(Vec3d::Constant(double(.6f)),0,1,{0,0,1},false,512);
    CHECK((expected-area_reference(Vec3d::Constant(double(.6f)),0,1,{0,0,1},false,256)).norm()<1e-5);
    const auto low=population(scene,s,r,1024,expected),high=population(scene,s,r,4096,expected);
    check_mean(high,expected); CHECK(high.mse<low.mse);
    r.initial_candidates=1; check_mean(population(scene,s,r,16384,expected),expected);
    r.initial_candidates=8;
    const auto full=estimate_restir_reference(scene,down,s,r,{0,192}).value();
    const auto first=estimate_restir_reference(scene,down,s,r,{0,37}).value(),last=estimate_restir_reference(scene,down,s,r,{37,155}).value();
    CHECK((full.mean-(first.mean*37+last.mean*155)/192).norm()<1e-12);
    CHECK(full.candidate_evaluations==192*8); CHECK(full.selected_shadow_rays==192);
    CHECK(full.statistics.rays<=192*4); CHECK(full.statistics.triangle_tests<=192*4*scene.triangle_count());
    CHECK((full.second_moment-(first.second_moment*37+last.second_moment*155)/192).norm()<1e-12);
}
TEST_CASE("RESTIR CPU black background camera emission and delta sun are not lost") {
    Fixture empty; auto s=settings(); s.environment_rgb={.2,.4,.7};
    CHECK(estimate_restir_reference(empty.scene(),down,s,{}, {0,1})->mean==s.environment_rgb);
    Fixture f; f.quad(-8,8,0,{.6f,.6f,.6f},{.1f,.2f,.3f}); auto scene=f.scene();
    s.environment_rgb.setZero(); CHECK((estimate_restir_reference(scene,down,s,{}, {0,1})->mean-Vec3d{double(.1f),double(.2f),double(.3f)}).norm()<1e-12);
    for(auto& m:f.asset.materials) m.emissive.setZero();
    CHECK(estimate_restir_reference(f.scene(),down,s,{}, {0,8})->mean==Vec3d::Zero());
    EnvironmentDescription env; env.sun.direction={0,0,1}; env.sun.irradiance={2,3,4}; f.doc->set_environment(env).value();
    scene=f.scene(); s.sun=PathTraceSun::scene;
    const Vec3d expected=Vec3d{2,3,4}*(double(.6f)/path_oracle::pi);
    CHECK((estimate_restir_reference(scene,down,s,{}, {0,1})->mean-expected).norm()<1e-12);
    s.environment_rgb=Vec3d::Constant(.3);
    check_mean(population(scene,s,{},16384,expected+Vec3d::Constant(.3*double(.6f))),expected+Vec3d::Constant(.3*double(.6f)));
}
TEST_CASE("RESTIR CPU mask emitter continuation and current occlusion retain energy") {
    Fixture f; f.quad(-8,8,0,{.6f,.6f,.6f}); f.quad(-1,1,2,{0,0,0},{2,2,2},true);
    for(std::size_t i=2;i<f.asset.materials.size();++i) f.asset.materials[i].alpha_mode=AlphaMode::mask;
    auto scene=f.scene(); auto s=settings();
    const Vec3d expected=area_reference(Vec3d::Constant(double(.6f)),0,1,{0,0,1},false,512);
    check_mean(population(scene,s,{},65536,expected),expected);
    f.quad(-4,4,1.5,{0,0,0},{0,0,0},true);
    for(std::size_t i=2;i<4;++i) f.asset.materials[i].alpha_mode=AlphaMode::opaque;
    scene=f.scene(); const auto blocked=estimate_restir_reference(scene,down,s,{}, {0,256}); REQUIRE(blocked);
    CHECK(blocked->mean==Vec3d::Zero()); CHECK(blocked->selected_shadow_rays==256);
    for(std::size_t i=4;i<6;++i) { f.asset.materials[i].alpha_mode=AlphaMode::mask; f.asset.materials[i].base_color.w()=0; }
    check_mean(population(f.scene(),s,{},4096,expected),expected);
}
TEST_CASE("RESTIR CPU GGX grazing area and narrow high light match independent integrals") {
    for(auto [rough,cosine]:{std::pair{.5,1.},std::pair{.3,.2},std::pair{.05,1.}}) {
        Fixture f; f.quad(-8,8,0,{.7f,.4f,.2f}); f.quad(-1,1,2,{0,0,0},{2,2,2},true);
        for(std::size_t i=0;i<2;++i) { f.asset.materials[i].metallic=.5f; f.asset.materials[i].roughness=float(rough); }
        const auto scene=f.scene(PathTraceMaterialModel::metallic_roughness); const Vec3d color{double(.7f),double(.4f),double(.2f)};
        const Vec3d wo{std::sqrt(1-cosine*cosine),0,cosine}; const PathTraceRay ray{wo,-wo};
        const int size=rough<.1 ? 2048 : 512;
        const auto expected=area_reference(color,.5,double(float(rough)),wo,true,size);
        const auto coarse=area_reference(color,.5,double(float(rough)),wo,true,size/2);
        for(int k=0;k<3;++k) REQUIRE(std::abs(expected[k]-coarse[k])<=.005*std::abs(expected[k])+1e-5);
        // A narrow lobe is deliberately a high-variance light-only proposal: report it, never clamp it.
        auto r=RestirDiSettings{}; r.initial_candidates=rough<.1 ? 64 : 8;
        check_mean(population(scene,settings(),r,65536,expected,false,ray),expected);
    }
}
TEST_CASE("RESTIR CPU secondary reflection matches classic MIS and preserves RR") {
    Fixture f; f.quad(-8,8,0,{.6f,.5f,.4f}); f.quad(-1,1,2,{.3f,.4f,.5f},{2,1,3},true);
    const auto scene=f.scene(); auto s=settings(4); s.environment_rgb={.1,.2,.3};
    const auto classic=population(scene,s,{},65536,Vec3d::Zero(),true);
    const auto ris=population(scene,s,{},32768,classic.mean); check_mean(ris,classic.mean,classic.se);
    s.rr_start=1; check_mean(population(scene,s,{},32768,classic.mean),classic.mean,classic.se);
}
TEST_CASE("RESTIR CPU mixed continuous lights and separate sun match analytic energy") {
    Fixture f; f.quad(-8,8,0,{.6f,.6f,.6f}); f.quad(-1,1,2,{0,0,0},{2,2,2},true);
    EnvironmentDescription environment;
    environment.sun.direction={std::sqrt(.5),0,std::sqrt(.5)}; environment.sun.irradiance={2,3,4};
    f.doc->set_environment(environment).value(); const auto scene=f.scene(); auto s=settings();
    s.sun=PathTraceSun::scene; s.environment_rgb={.3,.2,.1};
    const Vec3d area=area_reference(Vec3d::Constant(double(.6f)),0,1,{0,0,1},false,512);
    // The emitter occludes that part of the environment; solar direction misses the square.
    const Vec3d expected=area+s.environment_rgb*double(.6f)-area.cwiseProduct(s.environment_rgb)/2+
        Vec3d{2,3,4}*(double(.6f)*std::sqrt(.5)/path_oracle::pi);
    check_mean(population(scene,s,{},32768,expected),expected);
    check_mean(population(scene,s,{},32768,expected,true),expected);
}
TEST_CASE("RESTIR CPU active cancellation publishes no partial estimate") {
    Fixture f; f.quad(-8,8,0,{.6f,.6f,.6f}); f.quad(-1,1,2,{0,0,0},{2,2,2},true);
    const auto scene=f.scene(); auto s=settings(); RestirDiSettings r; r.initial_candidates=64;
    const auto before=estimate_restir_reference(scene,down,s,r,{0,1}).value();
    std::stop_source stop; Result<RestirReferenceEstimate> result;
    std::jthread work([&] { result=estimate_restir_reference(scene,down,s,r,{0,65536},0,stop.get_token()); });
    std::this_thread::sleep_for(std::chrono::milliseconds(2)); stop.request_stop(); work.join();
    REQUIRE_FALSE(result); CHECK(result.error().code==ErrorCode::invalid_state);
    CHECK(estimate_restir_reference(scene,down,s,r,{0,1})->mean==before.mean);
}
TEST_CASE("RESTIR CPU rejects budgets combinations cancellation and keeps frozen input") {
    Fixture f; f.quad(-8,8,0,{.6f,.6f,.6f}); const auto scene=f.scene(); auto s=settings(); s.environment_rgb=Vec3d::Ones();
    const auto before=estimate_restir_reference(scene,down,s,{}, {0,16}).value(); const auto revision=f.doc->revision();
    RestirDiSettings r; r.version=2; CHECK_FALSE(estimate_restir_reference(scene,down,s,r)); r={};
    for(unsigned n:{0u,65u}) { r.initial_candidates=n; CHECK_FALSE(estimate_restir_reference(scene,down,s,r)); } r={};
    r.max_candidate_evaluations=127; CHECK_FALSE(estimate_restir_reference(scene,down,s,r,{0,16})); r={};
    s.max_rays=63; CHECK_FALSE(estimate_restir_reference(scene,down,s,r,{0,16})); s.max_rays=1<<24;
    s.max_triangle_tests=127; CHECK_FALSE(estimate_restir_reference(scene,down,s,r,{0,16})); s.max_triangle_tests=1<<29;
    s.estimator=PathTraceEstimator::bsdf_only; CHECK_FALSE(estimate_restir_reference(scene,down,s)); s.estimator=PathTraceEstimator::mis;
    std::stop_source stop; stop.request_stop(); CHECK_FALSE(estimate_restir_reference(scene,down,s,{}, {0,16},0,stop.get_token()));
    CHECK_FALSE(estimate_restir_reference({},down,s)); CHECK_FALSE(estimate_restir_reference(scene,{{0,0,1},{0,0,-2}},s));
    CHECK(f.doc->revision()==revision); CHECK(estimate_restir_reference(scene,down,s,{}, {0,16})->mean==before.mean);
    f.asset.materials[0].base_color.x()=0; CHECK(estimate_restir_reference(scene,down,s,{}, {0,16})->mean==before.mean);
    EnvironmentDescription env; env.enabled=true; f.doc->set_environment(env).value();
    CHECK_FALSE(estimate_restir_reference(f.scene(),down,s)); s.surface_only=true; CHECK(estimate_restir_reference(f.scene(),down,s));
}
TEST_CASE("RESTIR CPU orthographic and perspective rays retain pixel sample identity") {
    Fixture f; f.quad(-8,8,0,{.6f,.6f,.6f}); const auto source=f.source(); const auto scene=f.scene();
    ViewDescription d{2,2}; Trsd pose; pose.translation={0,0,1}; d.camera_world=Transformd::from_trs(pose).value();
    auto s=settings(); s.environment_rgb=Vec3d::Ones();
    for(bool orthographic:{false,true}) {
        d.projection=orthographic ? orthographic_projection({2,.1,10},1).value() : perspective_projection({1,.1,10},1).value();
        const auto view=RenderView::create(source,d).value();
        for(unsigned i=0;i<4;++i) {
            const auto ray=path_trace_camera_ray(view,i%2,i/2,{.3,.7}).value();
            const auto actual=estimate_restir_reference(scene,ray,s,{}, {0,512},i); REQUIRE(actual);
            const auto expected=estimate_restir_reference(scene,down,s,{}, {0,512},i); REQUIRE(expected);
            CHECK((actual->mean-expected->mean).norm()<1e-12);
        }
    }
}
