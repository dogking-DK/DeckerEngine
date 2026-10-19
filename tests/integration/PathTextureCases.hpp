// Included after the probe's fixture/helpers, in the same translation unit.
namespace {
void add_texture(Fixture& f,std::size_t primitive,Vec2f uv={.25f,.25f}) {
    TextureData texture; texture.id=AssetId::generate().value(); texture.width=texture.height=2;
    texture.origin="procedural/baseColor"; texture.sampler.mag_filter=TextureFilter::nearest;
    texture.rgba8={std::byte{128},std::byte{0},std::byte{0},std::byte{0},std::byte{0},std::byte{255},std::byte{0},std::byte{255},
        std::byte{0},std::byte{0},std::byte{255},std::byte{128},std::byte{255},std::byte{255},std::byte{255},std::byte{255}};
    f.asset.materials[primitive].base_color_texture=texture.id;
    f.asset.mesh.primitives[primitive].texcoords={uv,uv,uv}; f.asset.textures.push_back(std::move(texture));
}
void texture_numeric(memory::ResourceHandle heap,PathTraceRenderer& renderer,SubmissionQueue& queue) {
    auto s=settings(); s.estimator=PathTraceEstimator::bsdf_only; s.environment_rgb=Vec3d::Ones();
    double worst=0;
    for(const auto filter:{TextureFilter::nearest,TextureFilter::linear}) for(const auto wrap:{TextureWrap::repeat,TextureWrap::clamp,TextureWrap::mirrored_repeat}) {
        Fixture f{heap}; std::vector<PathTraceRay> rays;
        const std::array<Vec2f,8> coordinates{Vec2f{.25,.25},Vec2f{.75,.25},Vec2f{.25,.75},Vec2f{.5,.5},Vec2f{-.25,1.25},Vec2f{1.25,-.25},Vec2f{0,.25},Vec2f{.37,.63}};
        for(std::size_t i=0;i<coordinates.size();++i) {
            const auto x=float(i)*4; f.triangle({x-1,-1,0},{x+1,-1,0},{x,1,0},{1,1,1}); add_texture(f,i,coordinates[i]);
            auto& texture=f.asset.textures.back(); texture.sampler.mag_filter=filter; texture.sampler.wrap_s=wrap; texture.sampler.wrap_t=wrap;
            rays.push_back({{x,0,1},{0,0,-1}});
        }
        const auto cpu=f.scene(); const auto scene=take(renderer.prepare_scene(queue,cpu));
        const auto gpu=complete(queue,take(renderer.submit_rays(queue,scene,rays,s,{0,16})));
        for(std::size_t i=0;i<rays.size();++i) {
            const auto expected=take(estimate_path_reference(cpu,rays[i],s,{0,16})).mean;
            worst=std::max(worst,(gpu.linear_rgb()[i]-expected).cwiseAbs().maxCoeff());
            close_color(gpu.linear_rgb()[i],expected,filter==TextureFilter::nearest ? .003 : .005);
        }
    }
    // Every lane selects a different descriptor, including the last valid slot.
    Fixture f{heap}; std::vector<PathTraceRay> rays;
    for(unsigned i=0;i<65;++i) {
        const auto x=float(i)*4; f.triangle({x-1,-1,0},{x+1,-1,0},{x,1,0},{1,1,1}); add_texture(f,i,{.75,.75});
        auto& t=f.asset.textures.back(); t.width=t.height=1;
        t.rgba8={std::byte(i*4),std::byte(255-i*3),std::byte(i*2),std::byte(i%2 ? 255 : 0)};
        if(i==63) {
            const auto pixel=t.rgba8; t.width=4097; t.rgba8.resize(4*t.width);
            for(unsigned x=0;x<t.width;++x) std::copy(pixel.begin(),pixel.end(),t.rgba8.begin()+4*x);
        }
        f.asset.materials[i].alpha_mode=AlphaMode::mask; rays.push_back({{x,0,1},{0,0,-1}});
    }
    const std::array assets{&f.asset};
    const auto source=take(PathTraceScene::create(heap,f.source(),assets,desktop_path_scene_limits()));
    require(source.texture_count()==65,"texture growth not exercised");
    auto scene=take(renderer.prepare_scene(queue,source)); const auto gpu=complete(queue,take(renderer.submit_rays(queue,scene,rays,s,{0,4})));
    for(std::size_t i=0;i<rays.size();++i) {
        close_color(gpu.linear_rgb()[i],take(estimate_path_reference(source,rays[i],s,{0,4})).mean,.003);
        require(gpu.primary_hits()[i].has_value()==bool(i%2),"divergent mask sampled wrong descriptor");
    }
    std::printf("texture RGB max_abs=%.9g filters/wrap/divergent65=pass\n",worst);
}
void texture_mask(memory::ResourceHandle heap,PathTraceRenderer& renderer,SubmissionQueue& queue) {
    Fixture f{heap}; f.quad(-2,2,0,{1,1,1}); f.quad(-2,2,.5,{0,0,0},{0,0,0},true);
    for(std::size_t i=2;i<4;++i) { add_texture(f,i); f.asset.materials[i].alpha_mode=AlphaMode::mask; f.asset.materials[i].double_sided=true; }
    EnvironmentDescription environment; environment.sun.direction={0,0,1}; environment.sun.irradiance={1,1,1}; f.document->set_environment(environment).value();
    auto s=settings(); s.sun=PathTraceSun::scene;
    const PathTraceRay floor{{0,0,.1},{0,0,-1},0,.2};
    auto cpu=f.scene(); auto scene=take(renderer.prepare_scene(queue,cpu));
    close_color(ray_image(renderer,queue,scene,s,1,floor).linear_rgb()[0],Vec3d::Constant(1/std::numbers::pi));
    require(ray_image(renderer,queue,scene,s,1).primary_hits()[0]->primitive<2,"radiance mask hole not ignored");
    for(std::size_t i=2;i<4;++i) for(auto& uv:f.asset.mesh.primitives[i].texcoords) uv={.75f,.75f};
    scene=take(renderer.prepare_scene(queue,f.scene())); close_color(ray_image(renderer,queue,scene,s,1,floor).linear_rgb()[0],Vec3d::Zero());
    for(std::size_t i=2;i<4;++i) f.asset.materials[i].emissive={2,3,4};
    cpu=f.scene(); require(cpu.masked_emitter_count()==2,"masked emitter diagnostics missing"); scene=take(renderer.prepare_scene(queue,cpu));
    const auto metadata=ray_image(renderer,queue,scene,s,1).info();
    require(metadata.texture_count==2 && metadata.masked_emitter_count==2,"frame material provenance missing");
    close_color(ray_image(renderer,queue,scene,s,1).linear_rgb()[0],{2,3,4});
    // Exact alpha factor=cutoff survives; opaque ignores alpha=0.
    for(std::size_t i=2;i<4;++i) f.asset.materials[i].base_color.w()=.5f;
    scene=take(renderer.prepare_scene(queue,f.scene())); close_color(ray_image(renderer,queue,scene,s,1).linear_rgb()[0],{2,3,4});
    for(std::size_t i=2;i<4;++i) { f.asset.materials[i].base_color.w()=0; f.asset.materials[i].alpha_mode=AlphaMode::opaque; }
    scene=take(renderer.prepare_scene(queue,f.scene())); close_color(ray_image(renderer,queue,scene,s,1).linear_rgb()[0],{2,3,4});
    Fixture sides{heap}; sides.triangle({-1,-1,0},{1,-1,0},{0,1,0},{0,0,0},{2,3,4}); add_texture(sides,0,{.75,.75}); sides.asset.materials[0].alpha_mode=AlphaMode::mask;
    const std::array rays{down,PathTraceRay{{0,0,-1},{0,0,1}}};
    for(const double scale:{1.0,-1.0}) for(const bool two:{false,true}) {
        Trsd transform; transform.scale={scale,2,.5}; sides.document->set_local_transform(sides.entity,transform).value(); sides.asset.materials[0].double_sided=two;
        const auto source=sides.scene(); const auto gpu=take(renderer.prepare_scene(queue,source));
        const auto image=complete(queue,take(renderer.submit_rays(queue,gpu,rays,settings(),{0,1})));
        for(std::size_t i=0;i<rays.size();++i) {
            const auto hit=take(source.nearest(rays[i])); require(bool(hit)==bool(image.primary_hits()[i]),"mask winding hit mismatch");
            if(hit) { require(hit->front_face==image.primary_hits()[i]->front_face,"mask front/back mismatch"); close_color(hit->normal,image.primary_hits()[i]->normal); }
        }
    }
    // A half-covered emitter has no NEE distribution; both estimators must retain BSDF emission.
    Fixture light{heap}; light.quad(-8,8,0,{.6f,.6f,.6f}); light.quad(-1,1,2,{0,0,0},{2,2,2},true);
    for(std::size_t i=2;i<4;++i) {
        add_texture(light,i); light.asset.materials[i].alpha_mode=AlphaMode::mask;
        auto& t=light.asset.textures.back(); t.width=2; t.height=1; t.rgba8={std::byte{255},std::byte{255},std::byte{255},std::byte{0},std::byte{255},std::byte{255},std::byte{255},std::byte{255}};
        for(std::size_t k=0;k<3;++k) light.asset.mesh.primitives[i].texcoords[k]={(light.asset.mesh.primitives[i].positions[k].x()+1)/2,.5f};
    }
    const auto emitter=take(renderer.prepare_scene(queue,light.scene())); s=settings(); const double expected=area_integral(256)*.5;
    const auto n=population(renderer,queue,emitter,s,16384,expected,true),high=population(renderer,queue,emitter,s,65536,expected,true);
    statistical(n,expected); statistical(high,expected); require(std::abs(n.mean-high.mean)/expected<.005,"mask emitter N/4N unconverged");
    s.estimator=PathTraceEstimator::bsdf_only; statistical(population(renderer,queue,emitter,s,65536,expected,true),expected);
    std::puts("radiance/visibility/mask emission/equality/opaque/mirrored sides=pass");
}
void texture_lifetime(memory::ResourceHandle heap,PathTraceRenderer& renderer,SubmissionQueue& queue) {
    Fixture f{heap}; f.triangle({-1,-1,0},{1,-1,0},{0,1,0},{1,1,1}); add_texture(f,0,{.75,.75});
    auto source=f.scene(); auto scene=take(renderer.prepare_scene(queue,source)); auto s=settings(); s.environment_rgb=Vec3d::Ones(); s.estimator=PathTraceEstimator::bsdf_only;
    const auto old=ray_image(renderer,queue,scene,s,1); const auto before=queue.stats().submitted;
    f.asset.textures[0].rgba8[12]=std::byte{0}; auto changed=f.scene(); require(changed.fingerprint()!=source.fingerprint(),"texture bytes absent from fingerprint");
    fail_submit=true; require(!renderer.prepare_scene(queue,changed),"failed texture upload published scene"); require(queue.stats().submitted==before,"failed upload advanced ticket");
    auto next=take(renderer.prepare_scene(queue,changed)); const auto current=ray_image(renderer,queue,next,s,1);
    require(current.linear_rgb()[0].x()<.001 && old.linear_rgb()[0].x()>.99,"texture edit corrupted old generation");
    auto pending=take(renderer.submit_rays(queue,next,std::span{&down,1},s,{0,128}));
    timeout_once=true; require(!take(pending.read(queue)),"texture pending published early");
    scene={}; next={}; source={}; changed={}; renderer={}; f.asset={}; check(queue.close());
    const auto final=take(pending.read(queue)); require(bool(final),"pending textures lost owners"); close_color(final->linear_rgb()[0],current.linear_rgb()[0]);
    close_color(old.linear_rgb()[0],Vec3d::Ones()); std::puts("texture upload failure/old generation/pending release/close=pass");
}
void texture_disabled(memory::ResourceHandle heap,PathTraceRenderer& renderer,SubmissionQueue& queue) {
    Fixture f{heap}; f.triangle({-1,-1,0},{1,-1,0},{0,1,0},{1,1,1}); add_texture(f,0);
    const auto before=queue.stats().submitted; const auto result=renderer.prepare_scene(queue,f.scene());
    require(!result && result.error().code==ErrorCode::not_supported,"disabled indexing accepted texture scene");
    require(queue.stats().submitted==before,"disabled texture request submitted work");
    const auto shader=take(compile_shader({std::filesystem::path(DK_PATH_SHADER_DIR)/"../../../../tests/fixtures/shaders/nonuniform.slang","computeMain",ShaderStage::compute},heap));
    require(!queue.resources().create_shader(shader),"disabled indexing accepted nonuniform shader");
    f.asset.materials[0].base_color_texture.reset(); static_cast<void>(ray_image(renderer,queue,take(renderer.prepare_scene(queue,f.scene())),settings(),1));
    std::puts("texture disabled scene/shader refusal and untextured fallback=pass");
}
#ifdef DK_PATH_ASSET_DIR
void texture_assets(memory::ResourceHandle heap,PathTraceRenderer& renderer,SubmissionQueue& queue) {
    const auto root=std::filesystem::current_path()/("pathtrace-assets-"+AssetId::generate().value().to_string());
    std::filesystem::create_directory(root);
    for(const auto name:{"pathtrace-mask.gltf","triangle.bin","rgba.png"})
        std::filesystem::copy_file(std::filesystem::path(DK_PATH_ASSET_DIR)/name,root/name);
    const auto paths=take(ProjectPaths::create(root)); const auto compiled=take(compile_asset(paths,{"pathtrace-mask.gltf","artifact"}));
    auto artifact=take(load_cpu_artifact(root/"artifact"));
    require(artifact.source=="pathtrace-mask.gltf" && artifact.inputs.size()==3,"disk input provenance missing");
    Fixture f{heap}; f.asset=std::move(artifact.data); f.document->set_asset_references(f.entity,{{f.asset.mesh.id,AssetKind::mesh}}).value();
    require(f.asset.mesh.id==compiled.root_id && f.asset.materials[0].alpha_mode==AlphaMode::mask && f.asset.materials[0].metallic==0,"disk material semantics changed");
    const auto source=f.scene(); require(source.texture_count()==1 && !source.texture_info(0)->origin.empty(),"disk texture provenance missing");
    const auto scene=take(renderer.prepare_scene(queue,source)); auto s=settings(); s.estimator=PathTraceEstimator::bsdf_only; s.environment_rgb=Vec3d::Ones();
    const auto& p=f.asset.mesh.primitives[0]; std::vector<PathTraceRay> rays;
    for(int a=1;a<6;++a) for(int b=1;b<6-a;++b) {
        const Vec3f point=p.positions[0]*(float(a)/6)+p.positions[1]*(float(b)/6)+p.positions[2]*(float(6-a-b)/6);
        rays.push_back({point.cast<double>()+Vec3d{0,0,1},{0,0,-1}});
    }
    auto frame=take(renderer.submit_rays(queue,scene,rays,s,{0,4}));
    // Mutate only this test's copy after freezing: GPU must never reread the file.
    { std::ofstream changed(root/"rgba.png",std::ios::binary|std::ios::trunc); changed<<"invalid PNG"; }
    require(!compile_asset(paths,{"pathtrace-mask.gltf","invalid"}),"corrupt new import unexpectedly succeeded");
    const auto gpu=complete(queue,frame); std::size_t hits=0,misses=0;
    for(std::size_t i=0;i<rays.size();++i) {
        close_color(gpu.linear_rgb()[i],take(estimate_path_reference(source,rays[i],s,{0,4})).mean,.003);
        const auto hit=take(source.nearest(rays[i])); require(bool(hit)==bool(gpu.primary_hits()[i]),"disk mask CPU/GPU mismatch");
        if(hit) { ++hits; require(gpu.primary_hits()[i]->mesh==compiled.root_id,"disk hit identity mismatch"); } else ++misses;
    }
    require(hits && misses,"disk fixture did not exercise mask coverage");
    std::printf("disk glTF -> CPU artifact -> RT hit=%zu miss=%zu fingerprint=%llu\n",hits,misses,static_cast<unsigned long long>(source.fingerprint()));
}
#endif
} // namespace
