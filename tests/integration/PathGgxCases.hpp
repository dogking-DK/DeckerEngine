#include "../PathBsdfOracle.hpp"
namespace {
PathTraceScene ggx_scene(const Fixture& f) {
    const std::array assets{&f.asset}; return take(PathTraceScene::create(f.heap,f.source(),assets,{},PathTraceMaterialModel::metallic_roughness));
}
void ggx_material(Fixture& f,float metal,float rough) { for(auto& m:f.asset.materials) { m.metallic=metal; m.roughness=rough; } }
void ggx_numeric(memory::ResourceHandle heap,PathTraceRenderer& renderer,SubmissionQueue& queue) {
    Fixture constant{heap}; constant.quad(-10,10,0,{.7f,.4f,.2f}); auto constant_settings=settings();
    constant_settings.estimator=PathTraceEstimator::bsdf_only; constant_settings.environment_rgb=Vec3d::Ones();
    const auto constant_image=ray_image(renderer,queue,take(renderer.prepare_scene(queue,constant.scene())),constant_settings,65536);
    const Vec3d constant_color=Vec3f{.7f,.4f,.2f}.cast<double>();
    close_color(constant_image.linear_rgb()[0],constant_color,1e-6);
    close_color(constant_image.second_moment()[0],constant_color.cwiseProduct(constant_color),1e-6);
    double worst=0;
    for(float metal:{0.f,.5f,1.f}) for(float rough:{0.f,.05f,.2f,.5f,1.f}) {
        Fixture f{heap}; f.quad(-100,100,0,{.7f,.4f,.2f}); ggx_material(f,metal,rough);
        for(double cosine:{1.,.1,.01}) {
            const Vec3d wo{std::sqrt(1-cosine*cosine),0,cosine};
            // Reflection direction places even the narrow lobe on the delta sun.
            const Vec3d light{-wo.x(),0,wo.z()}; EnvironmentDescription env; env.sun.direction={light.x(),0,light.z()}; env.sun.irradiance={1,1,1}; f.document->set_environment(env).value();
            const auto cpu=ggx_scene(f); const auto scene=take(renderer.prepare_scene(queue,cpu));
            const PathTraceRay ray{wo,-wo}; auto s=settings(); s.sun=PathTraceSun::scene;
            const Vec3d expected=path_oracle::evaluate(Vec3f{.7f,.4f,.2f}.cast<double>(),metal,rough,wo,light)*light.z();
            const auto actual=ray_image(renderer,queue,scene,s,1,ray);
            const double error=(actual.linear_rgb()[0]-expected).cwiseAbs().maxCoeff()/std::max(1e-2,expected.maxCoeff()); worst=std::max(worst,error);
            require(error<=1e-3,"GGX analytic delta sun mismatch");
            close_color(take(estimate_path_reference(cpu,ray,s,{0,1})).mean,expected,1e-8*std::max(1.,expected.maxCoeff()));
            require(actual.info().material_model==PathTraceMaterialModel::metallic_roughness && actual.info().settings.model_version==3,"GGX image model missing");
        }
    }
    Fixture black{heap}; black.quad(-5,5,0,{0,0,0}); ggx_material(black,0,1);
    EnvironmentDescription sun; sun.sun.direction={0,0,1}; sun.sun.irradiance={1,1,1}; black.document->set_environment(sun).value(); auto s=settings(); s.sun=PathTraceSun::scene;
    close_color(ray_image(renderer,queue,take(renderer.prepare_scene(queue,ggx_scene(black))),s,1).linear_rgb()[0],Vec3d::Constant(.04/(4*path_oracle::pi)),1e-6);
    // A missing material uses the asset's metallic=1, roughness=1 defaults.
    for(auto& p:black.asset.mesh.primitives) p.material.reset(); auto defaults=take(renderer.prepare_scene(queue,ggx_scene(black)));
    s=settings(); s.environment_rgb=Vec3d::Ones(); s.estimator=PathTraceEstimator::bsdf_only;
    close_color(ray_image(renderer,queue,defaults,s,65536).linear_rgb()[0],Vec3d::Constant(1-std::log(2.)),.004);
    std::printf("GGX analytic metallic/roughness endpoints normal/grazing black/default max_relative=%.9g\n",worst);
}
Population ggx_population(PathTraceRenderer& renderer,SubmissionQueue& queue,const PathTraceGpuScene& scene,PathTraceSettings s,
    std::uint32_t count,double expected,const PathTraceRay& ray=down) {
    Population p; std::array<double,8> values{}; double error=0,base=0,cpu_mean=0;
    for(std::uint32_t seed=0;seed<8;++seed) {
        s.seed=101+7919*seed; const auto gpu=ray_image(renderer,queue,scene,s,count,ray).linear_rgb()[0];
        const auto cpu=take(estimate_path_reference(scene.source(),ray,s,{0,count})).mean;
        cpu_mean+=cpu.x()/8;
        values[seed]=gpu.x(); p.mean+=gpu.x()/8; p.mse+=(gpu.x()-expected)*(gpu.x()-expected)/8;
        error+=(gpu-cpu).squaredNorm(); base+=cpu.squaredNorm();
    }
    for(double v:values) p.error+=(v-p.mean)*(v-p.mean)/56;
    p.error=std::sqrt(p.error); p.cpu_error=std::sqrt(error/std::max(base,1e-12)); std::printf("CPU population=%.9g\n",cpu_mean); return p;
}
void ggx_statistics(memory::ResourceHandle heap,PathTraceRenderer& renderer,SubmissionQueue& queue) {
    const std::array<std::array<float,3>,7> cases{{{0,.2f,1},{.5f,.5f,1},{1,1,1},{0,1,.1f},{1,.2f,.1f},{1,0,1},{1,0,.1f}}};
    for(auto item:cases) {
        const auto metal=item[0],rough=item[1]; const double cosine=item[2]; const Vec3d wo{std::sqrt(1-cosine*cosine),0,cosine};
        Fixture f{heap}; f.quad(-100,100,0,{.7f,.4f,.2f}); ggx_material(f,metal,rough);
        const auto cpu=ggx_scene(f); const auto scene=take(renderer.prepare_scene(queue,cpu)); auto s=settings(); s.environment_rgb=Vec3d::Ones();
        const Vec3d color=Vec3f{.7f,.4f,.2f}.cast<double>();
        const double expected=path_oracle::furnace(color,metal,rough,wo,768).x();
        require(std::abs(path_oracle::furnace(color,metal,rough,wo,384).x()-expected)<.001*expected,"GGX furnace quadrature not converged");
        const PathTraceRay ray{wo,-wo}; std::printf("GGX furnace m=%g r=%g cosine=%g\n",metal,rough,cosine);
        const auto low=ggx_population(renderer,queue,scene,s,16384,expected,ray),high=ggx_population(renderer,queue,scene,s,65536,expected,ray);
        statistical(low,expected); statistical(high,expected); require(std::abs(low.mean-high.mean)<=.005*expected,"GGX furnace N/4N unconverged");
        s.estimator=PathTraceEstimator::bsdf_only; statistical(ggx_population(renderer,queue,scene,s,65536,expected,ray),expected);
    }
    for(auto item:std::array<std::array<float,2>,3>{{{0,.2f},{.5f,.5f},{1,1}}}) {
        Fixture f{heap}; f.quad(-8,8,0,{.6f,.6f,.6f}); ggx_material(f,item[0],item[1]); f.quad(-1,1,2,{0,0,0},{2,2,2},true);
        const auto scene=take(renderer.prepare_scene(queue,ggx_scene(f))); auto s=settings();
        const double expected=2*path_oracle::area(Vec3d::Constant(double(.6f)),item[0],item[1],Vec3d::UnitZ(),768).x();
        require(std::abs(2*path_oracle::area(Vec3d::Constant(double(.6f)),item[0],item[1],Vec3d::UnitZ(),384).x()-expected)<.001*expected,"GGX area quadrature not converged");
        std::printf("GGX area m=%g r=%g\n",item[0],item[1]);
        const auto low=ggx_population(renderer,queue,scene,s,16384,expected),high=ggx_population(renderer,queue,scene,s,65536,expected);
        statistical(low,expected); statistical(high,expected); require(std::abs(low.mean-high.mean)<=.005*expected,"GGX area N/4N unconverged");
        s.estimator=PathTraceEstimator::bsdf_only; statistical(ggx_population(renderer,queue,scene,s,65536,expected),expected);
        s.rr_start=1; statistical(ggx_population(renderer,queue,scene,s,65536,expected),expected);
    }
}
void ggx_indirect(memory::ResourceHandle heap,PathTraceRenderer& renderer,SubmissionQueue& queue) {
    Fixture f{heap}; f.quad(-1000,1000,0,{.65f,.3f,.1f}); f.quad(-1000,1000,2,{.4f,.4f,.4f},{1,1,1},true);
    ggx_material(f,.5f,.5f); const auto cpu=ggx_scene(f),source=cpu; const auto scene=take(renderer.prepare_scene(queue,source));
    auto s=settings(4); s.estimator=PathTraceEstimator::bsdf_only;
    double expected=0; for(unsigned seed=0;seed<8;++seed) { s.seed=101+7919*seed; expected+=take(estimate_path_reference(cpu,down,s,{0,65536})).mean.x()/8; }
    const auto n=ggx_population(renderer,queue,scene,s,16384,expected),four=ggx_population(renderer,queue,scene,s,65536,expected);
    statistical(n,expected); statistical(four,expected); require(std::abs(n.mean-four.mean)<.005*expected,"GGX indirect N/4N unconverged");
    for(unsigned rr:{1u,3u}) { s.rr_start=rr; statistical(ggx_population(renderer,queue,scene,s,65536,expected),expected); }
    std::puts("GGX four bounces/colored transport/rr1/rr3=pass");
}
void ggx_lifetime(memory::ResourceHandle heap,PathTraceRenderer& renderer,SubmissionQueue& queue) {
    Fixture f{heap}; f.triangle({-3,-3,0},{3,-3,0},{0,3,0},{1,1,1}); ggx_material(f,.5f,.4f); add_texture(f,0,{.75f,.25f});
    f.asset.materials[0].alpha_mode=AlphaMode::mask;
    auto cpu=ggx_scene(f); auto scene=take(renderer.prepare_scene(queue,cpu)); auto s=settings(); s.environment_rgb=Vec3d::Ones();
    const auto old=ray_image(renderer,queue,scene,s,4096); const Vec3d old_value=old.linear_rgb()[0];
    close_color(old_value,take(estimate_path_reference(cpu,down,s,{0,4096})).mean,.003);
    f.asset.textures[0].rgba8[7]=std::byte{0};
    const auto hole=ray_image(renderer,queue,take(renderer.prepare_scene(queue,ggx_scene(f))),s,1);
    require(!hole.primary_hits()[0],"GGX alpha-mask hole was shaded"); close_color(hole.linear_rgb()[0],Vec3d::Ones());
    f.asset.textures[0].rgba8[7]=std::byte{255};
    auto whole=ray_image(renderer,queue,scene,s,256),first=ray_image(renderer,queue,scene,s,100),last=ray_image(renderer,queue,scene,s,156,down,100);
    close_color(whole.linear_rgb()[0],(first.linear_rgb()[0]*100+last.linear_rgb()[0]*156)/256,1e-6);
    const auto submitted=queue.stats().submitted; fail_submit=true; require(!renderer.prepare_scene(queue,cpu),"GGX failed upload published scene"); require(queue.stats().submitted==submitted,"GGX failed upload advanced ticket");
    f.asset.materials[0].metallic=1; f.asset.materials[0].roughness=1;
    auto changed=ggx_scene(f); require(changed.fingerprint()!=cpu.fingerprint(),"GGX material absent from fingerprint");
    auto next=take(renderer.prepare_scene(queue,changed)); auto pending=take(renderer.submit_rays(queue,next,std::span{&down,1},s,{0,4096}));
    const auto expected=take(estimate_path_reference(changed,down,s,{0,4096})).mean;
    timeout_once=true; require(!take(pending.read(queue)),"GGX pending published early");
    f.asset.materials[0].roughness=-1; const std::array assets{&f.asset}; require(!PathTraceScene::create(heap,f.source(),assets,{},PathTraceMaterialModel::metallic_roughness),"invalid GGX published");
    cpu={}; changed={}; scene={}; next={}; f.asset={}; renderer={}; check(queue.close());
    const auto result=take(pending.read(queue)); require(bool(result),"GGX pending owners lost"); close_color(result->linear_rgb()[0],expected,.003);
    require((old_value-result->linear_rgb()[0]).norm()>.05,"GGX material change not observable"); close_color(old.linear_rgb()[0],old_value);
    std::puts("GGX texture/mask/interval/failed upload/old material/pending release/close=pass");
}
}
