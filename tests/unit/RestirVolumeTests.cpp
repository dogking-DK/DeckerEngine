#include "../RestirTestSupport.hpp"
#include "../PathBsdfOracle.hpp"
#include <dk/render/VolumePathReference.hpp>
#include <dk/math/Projection.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <chrono>
#include <iostream>
#include <thread>
using namespace restir_test;
namespace {
VolumePathSettings volume_settings(unsigned depth=1) { VolumePathSettings s; s.max_bounces=depth; s.rr_start=0; s.sun=false; return s; }
EnvironmentDescription volume_air(Vec3d extinction,Vec3d scattering=Vec3d::Zero()) {
    EnvironmentDescription e; e.enabled=true; auto& a=e.atmosphere;
    a.atmosphere_height_m=10000; a.mie_scale_height_m=10000;
    a.absorption_center_height_m=5000; a.absorption_half_width_m=2500;
    for(unsigned c=0;c<3;++c) { a.mie_extinction_per_m[c]=extinction[c]; a.mie_scattering_per_m[c]=scattering[c]; }
    a.rayleigh_scattering_per_m=a.absorption_extinction_per_m={0,0,0}; a.mie_anisotropy=0; a.ground_albedo={0,0,0}; return e;
}
struct Population { Vec3d mean=Vec3d::Zero(),se=Vec3d::Zero(); double mse=0; RestirVolumeStatistics ris; VolumePathStatistics path; };
Population population(const PathTraceScene& scene,const PathMediumSnapshot& medium,const PathTraceRay& ray,VolumePathSettings settings,
    unsigned count=32768,bool classic=false,const RestirDiSettings& ris={},const Vec3d& truth=Vec3d::Zero()) {
    Population p; std::array<Vec3d,8> values;
    for(unsigned i=0;i<8;++i) {
        settings.seed=101+7919*i;
        if(classic) { auto r=estimate_volume_path_reference(scene,medium,ray,settings,{0,count}); REQUIRE(r); values[i]=r->mean; }
        else {
            auto r=estimate_restir_volume_reference(scene,medium,ray,settings,ris,{0,count});
            INFO((r ? "RIS volume succeeded" : r.error().message)); REQUIRE(r); values[i]=r->mean;
            p.ris.prepared_mesh+=r->restir_statistics.prepared_mesh; p.ris.prepared_ground+=r->restir_statistics.prepared_ground;
            p.ris.candidate_evaluations+=r->restir_statistics.candidate_evaluations; p.ris.applied_surface+=r->restir_statistics.applied_surface;
            p.ris.primary_scatter+=r->restir_statistics.primary_scatter; p.ris.primary_absorption+=r->restir_statistics.primary_absorption;
            p.ris.continuation_scatter+=r->restir_statistics.continuation_scatter; p.ris.selected_shadow_rays+=r->restir_statistics.selected_shadow_rays;
            p.path.medium_scatter+=r->statistics.medium_scatter; p.path.multiple_scatter_paths+=r->statistics.multiple_scatter_paths;
        }
        p.mean+=values[i]/8; p.mse+=(values[i]-truth).squaredNorm()/24;
    }
    for(const auto& v:values) p.se+=(v-p.mean).cwiseAbs2()/56;
    p.se=p.se.cwiseSqrt(); return p;
}
void check_mean(const Population& p,const Vec3d& expected,const Vec3d& reference_se=Vec3d::Zero()) {
    std::cout<<"RIS volume mean="<<p.mean.transpose()<<" expected="<<expected.transpose()<<" SE="<<p.se.transpose()<<'\n';
    for(unsigned c=0;c<3;++c) {
        const double error=std::abs(p.mean[c]-expected[c]),se=std::sqrt(p.se[c]*p.se[c]+reference_se[c]*reference_se[c]);
        INFO("channel="<<c<<" error="<<error<<" reference="<<expected[c]<<" combined SE="<<se);
        CHECK(error<=.02*std::abs(expected[c])+1e-4); CHECK(error<=5*se+1e-4);
    }
}
Vec3d area_integral(const Vec3d& color,const Vec3d& emission,unsigned n) {
    double factor=0;
    for(unsigned y=0;y<n;++y) for(unsigned x=0;x<n;++x) {
        const double px=-1+2*(x+.5)/n,py=-1+2*(y+.5)/n,d2=px*px+py*py+4;
        factor+=16/(double(n)*n*d2*d2*std::numbers::pi);
    }
    return color.cwiseProduct(emission)*factor;
}
void horizontal(Fixture& f,float height,Vec3f color) {
    f.triangle({-50000,height,-50000},{0,height,50000},{50000,height,-50000},color);
}
RenderView volume_view(Fixture& f,unsigned size=2,bool orthographic=false) {
    ViewDescription d{size,size}; Trsd camera; camera.translation={0,0,1}; d.camera_world=Transformd::from_trs(camera).value();
    d.projection.setZero(); d.projection(0,0)=2; d.projection(1,1)=-2;
    d.projection(2,2)=10./(.1-10); d.projection(2,3)=1./(.1-10); d.projection(3,2)=-1;
    if(orthographic) d.projection=orthographic_projection({2,.1,10},1).value();
    return RenderView::create(f.source(),d).value();
}
}
TEST_CASE("RESTIR volume RGB hero survival and independent stochastic shadow preserve conditional expectation") {
    const std::array<Vec3d,2> F{Vec3d{0,4,0},Vec3d{2,0,7}};
    const std::array q{.8,.2}; const Vec3d survival{1,.25,.001};
    const std::array<Vec3d,2> shadow{Vec3d{.2,.3,.9},Vec3d{1,.7,.1}};
    Vec3d expectation=Vec3d::Zero(); const Vec3d expected=survival.cwiseProduct(F[0].cwiseProduct(shadow[0])+F[1].cwiseProduct(shadow[1]));
    // Exhaust two RIS proposals, replacement, uniform hero, survival, and Bernoulli shadow.
    for(unsigned a=0;a<2;++a) for(unsigned b=0;b<2;++b) {
        const double ta=F[a].dot(Vec3d{.2126,.7152,.0722}),tb=F[b].dot(Vec3d{.2126,.7152,.0722});
        const double wa=ta/q[a],wb=tb/q[b];
        for(unsigned choice=0;choice<2;++choice) {
            const double probability=(choice ? wb : wa)/(wa+wb); RisReservoir reservoir;
            REQUIRE(reservoir.stream(ta,q[a],0)); REQUIRE(reservoir.stream(tb,q[b],choice ? 0 : std::nextafter(1.,0.)));
            const unsigned light=choice ? b : a;
            for(unsigned c=0;c<3;++c) for(unsigned alive=0;alive<2;++alive) for(unsigned visible=0;visible<2;++visible) {
                const double p=q[a]*q[b]*probability/3*(alive ? survival[c] : 1-survival[c])*(visible ? shadow[light][c] : 1-shadow[light][c]);
                expectation[c]+=p*3*alive*visible*F[light][c]*reservoir.normalization();
            }
        }
    }
    CHECK((expectation-expected).norm()<1e-12);
    for(unsigned sample=0;sample<32;++sample) {
        const double ris=restir_reference_sample(101,7,sample,0,RestirRandomPurpose::replacement);
        for(auto stream:{VolumeSampleStream::channel,VolumeSampleStream::free_path,VolumeSampleStream::shadow_path,VolumeSampleStream::restir_shadow_path})
            CHECK(ris!=volume_path_sample(101,7,sample,0,stream));
        CHECK(volume_path_sample(101,7,sample,0,VolumeSampleStream::restir_shadow_path)!=volume_path_sample(101,7,sample,0,VolumeSampleStream::shadow_path));
    }
}
TEST_CASE("RESTIR volume vacuum Lambert GGX and finite sun overlap match independent energy") {
    Fixture f; f.quad(-8,8,0,{.4f,.6f,.8f}); auto s=volume_settings(); s.environment_rgb=Vec3d::Ones();
    auto m=PathMediumSnapshot::create({}).value(); const Vec3d color=Vec3f{.4f,.6f,.8f}.cast<double>();
    check_mean(population(f.scene(),m,down,s),color);
    RestirDiSettings one; one.initial_candidates=1; check_mean(population(f.scene(),m,down,s,65536,false,one),color);
    for(auto& material:f.asset.materials) { material.metallic=.7f; material.roughness=.4f; }
    auto expected=path_oracle::furnace(color,double(.7f),double(.4f),Vec3d::UnitZ(),512);
    const auto coarse=path_oracle::furnace(color,double(.7f),double(.4f),Vec3d::UnitZ(),256);
    for(unsigned c=0;c<3;++c) REQUIRE(std::abs(expected[c]-coarse[c])<=.005*expected[c]+1e-5);
    check_mean(population(f.scene(PathTraceMaterialModel::metallic_roughness),m,down,s,65536),expected);
    for(auto& material:f.asset.materials) material.metallic=0;
    EnvironmentDescription e; e.sun.direction={0,0,1}; e.sun.angular_radius_rad=.1; e.sun.irradiance={1,2,3}; f.doc->set_environment(e).value();
    s.sun=true; s.environment_rgb={.2,.3,.4}; m=PathMediumSnapshot::create(e).value();
    expected=color.cwiseProduct(s.environment_rgb+Vec3d{1,2,3}/std::numbers::pi);
    check_mean(population(f.scene(),m,down,s,65536),expected);
    s.environment_rgb.setZero(); f.quad(-100,100,2,{0,0,0},{0,0,0},true);
    CHECK(population(f.scene(),m,down,s,1024).mean.isZero(0));
    for(unsigned i=2;i<4;++i) { f.asset.materials[i].alpha_mode=AlphaMode::mask; f.asset.materials[i].base_color.w()=0; }
    check_mean(population(f.scene(),m,down,s),color.cwiseProduct(Vec3d{1,2,3})/std::numbers::pi);
}
TEST_CASE("RESTIR volume real primary scattering retains ordinary paths and surface continuation conserves energy") {
    Fixture f; auto e=volume_air(Vec3d::Constant(1e-5),Vec3d::Constant(1e-5)); e.atmosphere.ground_albedo={1,1,1};
    REQUIRE(f.doc->set_environment(e)); const auto scene=f.scene(); const auto m=PathMediumSnapshot::create(e).value();
    auto s=volume_settings(32); s.environment_rgb=Vec3d::Ones();
    const PathTraceRay primary{{0,8000,0},{0,-1,0}}; unsigned scattered=0,survived=0;
    for(unsigned i=0;i<1024;++i) {
        const auto ris=estimate_restir_volume_reference(scene,m,primary,s,{}, {i,1}).value();
        if(ris.restir_statistics.primary_scatter) {
            const auto classic=estimate_volume_path_reference(scene,m,primary,s,{i,1}).value(); ++scattered;
            CHECK(ris.mean==classic.mean); CHECK(ris.second_moment==classic.second_moment);
            CHECK(ris.statistics.tracking_steps==classic.statistics.tracking_steps);
            CHECK(ris.statistics.rays==classic.statistics.rays); CHECK(ris.restir_statistics.applied_surface==0);
        } else ++survived;
        CHECK(ris.restir_statistics.prepared_ground==1); CHECK(ris.restir_statistics.candidate_evaluations==8);
    }
    CHECK(scattered>0); CHECK(survived>0);
    const PathTraceRay near_ground{{0,1,0},{0,-1,0}};
    const auto n=population(scene,m,near_ground,s,16384,false,{},Vec3d::Ones());
    const auto four=population(scene,m,near_ground,s,65536,false,{},Vec3d::Ones());
    check_mean(n,Vec3d::Ones()); check_mean(four,Vec3d::Ones()); CHECK(four.mse<n.mse);
    CHECK(four.ris.continuation_scatter>1000); CHECK(four.path.multiple_scatter_paths>100);
    s.rr_start=2; check_mean(population(scene,m,near_ground,s,65536),Vec3d::Ones());
}
TEST_CASE("RESTIR volume real cloud shadow changes with frozen wind and scattering retains energy") {
    Fixture f; EnvironmentDescription e; e.clouds.enabled=true; e.clouds.coverage=1; e.sun.angular_radius_rad=1e-6;
    e.clouds.weather_resolution=e.clouds.base_resolution=e.clouds.detail_resolution=16;
    e.atmosphere.ground_albedo={.5,.5,.5}; REQUIRE(f.doc->set_environment(e));
    CloudFieldCache cache; const auto field=cache.prepare(e.clouds).value(); PathMediumSettings ms; ms.cloud_albedo=0;
    auto s=volume_settings(); s.sun=true; const auto scene=f.scene();
    // Ground at x=z=0 makes camera and sunlight normal exactly radial; no planar approximation.
    const PathTraceRay ray{{0,1,0},{0,-1,0}}; Vec3d previous=Vec3d::Zero();
    for(double time:{17.,2017.}) {
        ms.time_seconds=time; const auto m=PathMediumSnapshot::create(e,ms,field).value();
        const auto fine=m.integrate({{0,0,0},{0,1,0},100000},{8192,40960}).value().transmittance;
        const auto coarse=m.integrate({{0,0,0},{0,1,0},100000},{4096,40960}).value().transmittance;
        for(unsigned c=0;c<3;++c) REQUIRE(std::abs(fine[c]-coarse[c])<=.005*fine[c]+1e-5);
        if(time==17) REQUIRE(fine.x()<.99); // Wind may move a clear gap over this receiver later.
        check_mean(population(scene,m,ray,s,65536),fine*(.5/std::numbers::pi));
        if(time>17) CHECK((fine-previous).norm()>1e-3);
        previous=fine;
        const auto a=estimate_restir_volume_reference(scene,m,ray,s,{}, {0,128}).value();
        const auto b=estimate_restir_volume_reference(scene,m,ray,s,{}, {0,128}).value(); CHECK(a.mean==b.mean);
    }
    e.atmosphere.ground_albedo={1,1,1}; REQUIRE(f.doc->set_environment(e)); ms.cloud_albedo=1; ms.time_seconds=17;
    auto mixed=volume_air(Vec3d::Constant(1e-5),Vec3d::Constant(1e-5)); mixed.clouds=e.clouds; mixed.atmosphere.ground_albedo={1,1,1};
    for(const auto& environment:{e,mixed}) {
        REQUIRE(f.doc->set_environment(environment)); const auto m=PathMediumSnapshot::create(environment,ms,field).value();
        s=volume_settings(32); s.environment_rgb=Vec3d::Ones(); s.rr_start=3;
        const auto p=population(f.scene(),m,ray,s,65536); check_mean(p,Vec3d::Ones());
        CHECK(p.ris.continuation_scatter>1000); CHECK(p.path.multiple_scatter_paths>100);
    }
}
TEST_CASE("RESTIR volume planet ordering and direct camera emission keep unsupported domains out of reservoirs") {
    Fixture f; auto e=volume_air(Vec3d::Zero()); e.atmosphere.ground_albedo={.7,.3,.9}; REQUIRE(f.doc->set_environment(e));
    horizontal(f,1,{.4f,.5f,.6f}); auto m=PathMediumSnapshot::create(e).value(); auto s=volume_settings(); s.environment_rgb=Vec3d::Ones();
    const PathTraceRay ray{{0,2,0},{0,-1,0}};
    auto a=estimate_restir_volume_reference(f.scene(),m,ray,s,{}, {0,512}).value(); CHECK(a.restir_statistics.prepared_mesh==512);
    for(auto& p:f.asset.mesh.primitives) for(auto& v:p.positions) v.y()=-1;
    auto b=estimate_restir_volume_reference(f.scene(),m,ray,s,{}, {0,512}).value(); CHECK(b.restir_statistics.prepared_ground==512);
    CHECK_FALSE(estimate_restir_volume_reference(f.scene(),m,{{0,-1,0},{0,1,0}},s));
    // Planet occlusion is recomputed for the selected sun direction.
    e.sun.direction={0,-1,0}; REQUIRE(f.doc->set_environment(e)); m=PathMediumSnapshot::create(e).value();
    s.environment_rgb.setZero(); s.sun=true;
    CHECK(estimate_restir_volume_reference(f.scene(),m,ray,s,{}, {0,512})->mean.isZero(0));
    e.enabled=false; e.sun.direction={0,0,1}; REQUIRE(f.doc->set_environment(e)); m=PathMediumSnapshot::create(e).value();
    const auto scene=f.scene(); s=volume_settings(0); s.sun=true;
    for(const PathTraceRay camera:{PathTraceRay{{0,2,0},{0,0,1}},ray}) {
        const auto ris=estimate_restir_volume_reference(scene,m,camera,s,{}, {0,512}).value();
        const auto classic=estimate_volume_path_reference(scene,m,camera,s,{0,512}).value();
        CHECK(ris.mean==classic.mean); CHECK(ris.statistics.rays==classic.statistics.rays); CHECK(ris.restir_statistics.candidate_evaluations==0);
    }
}
TEST_CASE("RESTIR volume area and mask emission partition preserve secondary paths") {
    Fixture f; f.quad(-8,8,0,{.6f,.4f,.2f}); f.quad(-1,1,2,{0,0,0},{2,3,4},true);
    auto m=PathMediumSnapshot::create({}).value(); auto s=volume_settings(); const Vec3d color=Vec3f{.6f,.4f,.2f}.cast<double>();
    const auto expected=area_integral(color,{2,3,4},512),coarse=area_integral(color,{2,3,4},256);
    REQUIRE((expected-coarse).cwiseAbs().maxCoeff()<1e-5);
    check_mean(population(f.scene(),m,down,s),expected);
    for(unsigned i=2;i<4;++i) f.asset.materials[i].alpha_mode=AlphaMode::mask;
    auto masked=population(f.scene(),m,down,s,65536); check_mean(masked,expected); CHECK(masked.ris.candidate_evaluations==0);
    for(unsigned i=2;i<4;++i) { f.asset.materials[i].alpha_mode=AlphaMode::opaque; f.asset.materials[i].base_color={.3f,.4f,.5f,1}; }
    s=volume_settings(4); s.environment_rgb={.2,.3,.4}; s.rr_start=2; auto scene=f.scene();
    const auto classic=population(scene,m,down,s,65536,true); check_mean(population(scene,m,down,s,65536),classic.mean,classic.se);
}
TEST_CASE("RESTIR volume colored absorption retains unconditional mesh and ground candidates") {
    const Vec3d sigma{3e-5,1.5e-4,3e-4},color{double(.7f),double(.3f),double(.9f)};
    for(bool ground:{false,true}) {
        Fixture f; auto e=volume_air(sigma); e.sun.angular_radius_rad=1e-4; e.sun.direction={0,1,0}; e.sun.irradiance={1,.8,.6};
        const double h=ground ? 0 : 1000,eye=8000;
        if(ground) e.atmosphere.ground_albedo={color.x(),color.y(),color.z()}; else horizontal(f,1000,color.cast<float>());
        f.doc->set_environment(e).value(); const auto scene=f.scene(); const auto m=PathMediumSnapshot::create(e).value();
        auto s=volume_settings(); s.sun=true; const PathTraceRay ray{{0,eye,0},{0,-1,0}};
        Vec3d expected;
        for(unsigned c=0;c<3;++c) {
            const double tau=sigma[c]*10000*(2*std::exp(-h/10000)-std::exp(-eye/10000)-std::exp(-1.));
            expected[c]=color[c]*e.sun.irradiance[c]/std::numbers::pi*std::exp(-tau);
        }
        auto actual=population(scene,m,ray,s,65536); check_mean(actual,expected);
        CHECK(actual.ris.candidate_evaluations==8ull*8*65536); CHECK(actual.ris.primary_absorption>0); CHECK(actual.ris.primary_scatter==0);
        CHECK(actual.ris.applied_surface+actual.ris.primary_absorption==8ull*65536);
        CHECK(actual.ris.prepared_ground==(ground ? 8ull*65536 : 0)); CHECK(actual.ris.prepared_mesh==(ground ? 0 : 8ull*65536));
        CHECK(actual.ris.selected_shadow_rays<=actual.ris.applied_surface);
    }
}
TEST_CASE("RESTIR volume sample partitions jittered images and immutable provenance agree") {
    Fixture f; f.quad(-8,8,0,{.5f,.6f,.7f}); auto scene=f.scene(); auto m=PathMediumSnapshot::create({}).value();
    auto s=volume_settings(); s.environment_rgb={1,.5,.25}; RestirDiSettings ris; ris.initial_candidates=4;
    const auto a=estimate_restir_volume_reference(scene,m,down,s,ris,{0,1024},7).value();
    const auto b=estimate_restir_volume_reference(scene,m,down,s,ris,{1024,1024},7).value();
    const auto all=estimate_restir_volume_reference(scene,m,down,s,ris,{0,2048},7).value();
    CHECK((all.mean-(a.mean+b.mean)*.5).norm()<1e-14); CHECK((all.second_moment-(a.second_moment+b.second_moment)*.5).norm()<1e-14);
    CHECK(all.restir_statistics.candidate_evaluations==a.restir_statistics.candidate_evaluations+b.restir_statistics.candidate_evaluations);
    CHECK(all.restir.initial_candidates==4); CHECK(all.samples.count==2048);
    VolumePathImage retained; auto last=volume_view(f);
    for(bool orthographic:{false,true}) {
        const auto view=volume_view(f,2,orthographic); last=view;
        auto result=render_restir_volume_reference(f.heap,scene,m,view,s,ris,{11,32}); REQUIRE(result); const auto image=*result;
        CHECK(image.info().restir->initial_candidates==4); CHECK(image.info().samples.first==11);
        CHECK(image.info().scene_fingerprint==scene.fingerprint()); CHECK(image.info().scene==scene.id()); CHECK(image.info().revision==scene.revision());
        CHECK(image.info().environment==m.environment()); CHECK(image.info().settings.model_version==4); CHECK(image.info().settings.sampler_version==2);
        CHECK(image.info().restir_statistics.applied_surface==128); CHECK(image.info().restir_statistics.candidate_evaluations==512);
        CHECK(image.info().statistics.rays<=128*3); // Cached primary geometry query, one fresh selected shadow.
        for(unsigned y=0;y<2;++y) for(unsigned x=0;x<2;++x) {
            const auto pixel=y*2+x; Vec3d sum=Vec3d::Zero(),square=Vec3d::Zero();
            for(unsigned sample=11;sample<43;++sample) {
                const Vec2d jitter{volume_path_sample(s.seed,pixel,sample,0,VolumeSampleStream::pixel),volume_path_sample(s.seed,pixel,sample,0,VolumeSampleStream::pixel,1)};
                const auto ray=path_trace_camera_ray(view,x,y,jitter).value();
                const auto fixed=estimate_restir_volume_reference(scene,m,ray,s,ris,{sample,1},pixel).value(); sum+=fixed.mean; square+=fixed.second_moment;
            }
            CHECK((image.linear_rgb()[pixel]-sum/32).norm()<1e-14); CHECK((image.second_moments()[pixel]-square/32).norm()<1e-14);
        }
        CHECK_FALSE(render_volume_path_reference(f.heap,scene,m,view,s,{0,1})->info().restir); retained=image;
    }
    const Vec3d old=retained.linear_rgb()[0]; f.doc.reset(); f.asset={}; f.heap.begin_close();
    CHECK_FALSE(render_restir_volume_reference(f.heap,scene,m,last,s,ris,{0,1})); scene={}; m={};
    CHECK(retained.linear_rgb()[0]==old); CHECK(retained.info().settings.environment_rgb==s.environment_rgb);
}
TEST_CASE("RESTIR volume validation budgets cancellation and allocation failure publish no partial image") {
    Fixture f; f.quad(-8,8,0,{.5f,.6f,.7f}); const auto scene=f.scene(); const auto m=PathMediumSnapshot::create({}).value();
    const auto view=volume_view(f,8); auto s=volume_settings(); s.environment_rgb=Vec3d::Ones();
    for(unsigned mode=0;mode<8;++mode) {
        RestirDiSettings r;
        switch(mode) {
        case 0:r.version=2;break; case 1:r.initial_candidates=0;break; case 2:r.initial_candidates=65;break;
        case 3:r.temporal=RestirTemporalSettings{};break; case 4:r.spatial=RestirSpatialSettings{};break;
        case 5:r.max_candidate_evaluations=0;break; case 6:r.max_candidate_evaluations=1ull<<25;break;
        case 7:r.max_candidate_evaluations=7;break;
        }
        CHECK_FALSE(estimate_restir_volume_reference(scene,m,down,s,r,{0,1}));
        const auto before=f.heap.snapshot(); CHECK_FALSE(render_restir_volume_reference(f.heap,scene,m,view,s,r,{0,1}));
        CHECK(f.heap.snapshot().allocation_count==before.allocation_count); // Preflight before output allocation.
    }
    auto bad=s; bad.estimator=PathTraceEstimator::bsdf_only; CHECK_FALSE(estimate_restir_volume_reference(scene,m,down,bad));
    CHECK_FALSE(estimate_restir_volume_reference(scene,m,down,s,{}, {65535,2})); CHECK_FALSE(estimate_restir_volume_reference({},m,down,s));
    bad=s; bad.max_rays=2; CHECK_FALSE(estimate_restir_volume_reference(scene,m,down,bad));
    bad=s; bad.max_triangle_tests=1; CHECK_FALSE(estimate_restir_volume_reference(scene,m,down,bad));
    const auto small=f.system.create_heap({"restir-volume-budget",memory::DomainCategory::render,32768}).value();
    std::vector<void*> blocks; while(auto p=small.try_allocate(256)) blocks.push_back(*p);
    bool failed=false,partial=false,success=false;
    for(;;) {
        const auto before=small.snapshot();
        { auto image=render_restir_volume_reference(small,scene,m,view,s,{}, {0,1}); failed|=!image; success|=bool(image);
          if(!image) partial|=small.snapshot().allocation_count>before.allocation_count+1; }
        CHECK(small.snapshot().live_allocations==before.live_allocations); CHECK(small.snapshot().backing_requested_bytes==before.backing_requested_bytes);
        if(blocks.empty()) break; const auto p=blocks.back(); blocks.pop_back(); small.deallocate(p,256);
    }
    CHECK(failed); CHECK(partial); CHECK(success);
    std::stop_source pre; pre.request_stop();
    auto stopped=estimate_restir_volume_reference(scene,m,down,s,{}, {0,1},0,pre.get_token()); REQUIRE_FALSE(stopped);
    CHECK(stopped.error().context==std::vector<std::string>{"render.pathtracing.volume.cancelled"});
    const auto before=f.heap.snapshot(); std::stop_source cancel;
    std::jthread stopper([&] { std::this_thread::sleep_for(std::chrono::milliseconds(10)); cancel.request_stop(); });
    const auto cancelled=render_restir_volume_reference(f.heap,scene,m,volume_view(f,16),s,{}, {0,4096},cancel.get_token()); REQUIRE_FALSE(cancelled);
    CHECK(cancelled.error().context==std::vector<std::string>{"render.pathtracing.volume.cancelled"});
    CHECK(f.heap.snapshot().live_allocations==before.live_allocations);
    REQUIRE(render_restir_volume_reference(f.heap,scene,m,view,s,{}, {0,1}));
}
TEST_CASE("RESTIR volume frozen cloud owners and exhausted tracking preserve accepted images") {
    Fixture f; auto e=volume_air(Vec3d::Constant(1e-5),Vec3d::Constant(1e-5)); e.clouds.enabled=true; e.clouds.coverage=1;
    e.clouds.weather_resolution=e.clouds.base_resolution=e.clouds.detail_resolution=16; REQUIRE(f.doc->set_environment(e));
    const auto scene=f.scene(); auto description=volume_view(f,4).description(); Trsd camera; camera.translation={0,2000,0};
    description.camera_world=Transformd::from_trs(camera).value(); const auto view=RenderView::create(f.source(),description).value();
    auto cache=std::make_unique<CloudFieldCache>(); auto field=cache->prepare(e.clouds).value(); std::weak_ptr<const CloudFieldData> weak=field;
    PathMediumSettings ms; ms.time_seconds=23; auto m=PathMediumSnapshot::create(e,ms,field).value(); auto s=volume_settings(8); s.sun=true;
    auto old=render_restir_volume_reference(f.heap,scene,m,view,s,{}, {0,16}).value(); const Vec3d color=old.linear_rgb()[0];
    const auto before=f.heap.snapshot(); auto bad=s; bad.max_tracking_steps=1;
    for(bool per_path:{false,true}) {
        if(per_path) { bad=s; bad.max_steps_per_path=1; }
        const auto rejected=render_restir_volume_reference(f.heap,scene,m,view,bad,{}, {0,16}); REQUIRE_FALSE(rejected);
        CHECK(rejected.error().message.find("tracking")!=std::string::npos); CHECK(f.heap.snapshot().live_allocations==before.live_allocations);
        CHECK(old.linear_rgb()[0]==color);
    }
    auto changed=e; changed.sun.irradiance={2,2,2}; CHECK_FALSE(estimate_restir_volume_reference(scene,PathMediumSnapshot::create(changed,ms,field).value(),{{0,2000,0},{0,1,0}},s));
    ++e.clouds.seed; REQUIRE(cache->prepare(e.clouds)); REQUIRE(f.doc->set_environment(e)); field.reset(); cache.reset(); m={};
    CHECK_FALSE(weak.expired()); CHECK(old.info().field_hash!=0); CHECK(old.info().medium.time_seconds==23);
    CHECK(old.info().environment.clouds.seed==1); CHECK(old.info().restir->version==1); CHECK(old.linear_rgb()[0]==color);
    old={}; CHECK(weak.expired());
}
