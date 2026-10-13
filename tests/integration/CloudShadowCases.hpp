#pragma once
// Uses the shared scene/GPU fixture; all shadow modes require real GPU validation.
CloudShadowRenderer shadow_renderer(Fixture& f,SubmissionQueue& q,CloudShadowLimits limits={}) {
    return take(CloudShadowRenderer::create(f.heap,q,std::filesystem::path{DK_RENDER_SHADER_DIR}/"../../engine/render/clouds/shaders",limits));
}
std::vector<float> shadow_pixels(const CloudShadowMap& map,SubmissionQueue& q) {
    require(take(q.wait(map.submission())),"shadow wait"); const auto side=take(map.settings()).resolution;
    std::vector<float> values(std::size_t{side}*side); check(map.read_transmittance(values));
    for(float value:values) require(std::isfinite(value) && value>=0 && value<=1,"invalid shadow T"); return values;
}
double shadow_reference(Fixture& f,const EnvironmentDescription& e,const CloudRay& ray,const CloudShadowSettings& s,unsigned steps) {
    CloudReferenceSettings r; r.steps=steps; r.sun_steps=r.atmosphere_steps=1; r.single_scattering_albedo=0;
    r.self_shadow=false; r.homogeneous=s.homogeneous; r.homogeneous_density=s.homogeneous_density; r.time_seconds=s.time_seconds;
    return take(integrate_cloud_reference(e,*f.data,ray,r)).transmittance;
}
void shadow_numeric(Fixture& f,SubmissionQueue& q) {
    auto e=cloudy(); e.clouds.extinction_per_m=.0003; CloudShadowSettings s; s.resolution=64; s.diagnostics=true;
    for(unsigned mode=0;mode<4;++mode) {
        auto owner=shadow_renderer(f,q); s.homogeneous=mode<2;
        e.sun.direction=mode==0 ? EnvironmentVector{0,1,0} : mode==3 ? EnvironmentVector{1,.02,0} : EnvironmentVector{.6,.8,0};
        e=take(normalize_environment(e)); f.prepare(q,e); const auto map=take(owner.prepare(q,e,*f.texture,s)); const auto values=shadow_pixels(map,q);
        double error=0,convergence=0;
        for(unsigned y=1;y<s.resolution;y+=13) for(unsigned x=1;x<s.resolution;x+=13) {
            const auto ray=take(cloud_shadow_ray(e,s,x,y)); const auto reference=shadow_reference(f,e,ray,s,4096);
            const double middle=shadow_reference(f,e,ray,s,2048),low=shadow_reference(f,e,ray,s,1024);
            convergence=std::max({convergence,std::abs(reference-middle),std::abs(middle-low)});
            const double expected=s.homogeneous ? std::exp(-e.clouds.extinction_per_m*e.clouds.density*s.homogeneous_density*ray.max_distance_m) : reference;
            error=std::max(error,std::abs(values[y*s.resolution+x]-expected));
        }
        std::printf("shadow column mode=%u maximum=%.9g convergence=%.9g\n",mode,error,convergence);
        require(error<=(s.homogeneous ? .002 : .02) && convergence<=.005,"shadow column reference threshold");
    }
    // One texel of +world-X wind must shift the same cloud feature, not its opposite.
    auto owner=shadow_renderer(f,q); e.sun.direction={0,1,0}; e.clouds.wind_velocity_mps={s.extent_m/s.resolution,0}; f.prepare(q,e);
    s.homogeneous=false; s.time_seconds=0; auto first=take(owner.prepare(q,e,*f.texture,s)); const auto before=shadow_pixels(first,q);
    require(take(owner.prepare(q,e,*f.texture,s)).generation()==first.generation(),"identical shadow key rebuilt");
    s.time_seconds=1; auto moved=take(owner.prepare(q,e,*f.texture,s)); const auto after=shadow_pixels(moved,q);
    const auto projection=take(moved.projection()); require(projection.u.x()<-0.99,"wind fixture light-space axis");
    double correct=0,wrong=0,change=0;
    for(unsigned y=0;y<s.resolution;++y) for(unsigned x=1;x+1<s.resolution;++x) {
        const auto i=y*s.resolution+x; correct=std::max(correct,std::abs(double(after[i])-before[i+1]));
        wrong=std::max(wrong,std::abs(double(after[i])-before[i-1])); change=std::max(change,std::abs(double(after[i])-before[i]));
    }
    std::printf("wind correct-shift=%.9g opposite-shift=%.9g changed=%.9g\n",correct,wrong,change);
    require(correct<.002 && wrong>correct*2 && change>.001,"cloud shadow wind direction or sampling");
    s.time_seconds=0; auto restored=take(owner.prepare(q,e,*f.texture,s)); require(shadow_pixels(restored,q)==before,"shadow time restore differs");
    require(shadow_pixels(first,q)==before,"old cloud shadow changed");
}
RenderFrame surface_shadow(Fixture& f,SubmissionQueue& q,const AtmosphereSky& sky,const CloudShadowMap* shadow) {
    auto frame=take(f.pipeline.render_sunlight(q,f.view(),f.assets,sky,f.settings,shadow)); require(take(frame.wait(q)),"shadow surface wait"); return frame;
}
void shadow_surface(Fixture& f,SubmissionQueue& q) {
    f.side=33; f.anchor={0,500,0}; f.rotation=Quatd::FromTwoVectors(Vec3d{0,0,-1},Vec3d{0,-1,0}); f.distance=400; f.place();
    f.settings.sky_diffuse=false; CloudShadowSettings s; s.resolution=128; s.diagnostics=true; s.homogeneous=true;
    {
        auto e=vacuum(); e.clouds.enabled=true; e.clouds.extinction_per_m=.0003; e.sun.direction={.6,.8,0}; auto sky=f.prepare(q,e);
        auto owner=shadow_renderer(f,q); auto map=take(owner.prepare(q,e,*f.texture,s));
        // Consume before waiting for the map: graph/ledger supplies storage-to-fragment ordering.
        const auto shaded=surface_shadow(f,q,sky,&map),unshadowed=surface_shadow(f,q,sky,nullptr);
        const auto a=linear(shaded),b=linear(unshadowed); const Vec3d receiver{0,100,0};
        const auto path=take(cloud_shell_intervals(e,{receiver,Vec3d{e.sun.direction.data()},200000})); double distance=0;
        for(unsigned i=0;i<path.count;++i) distance+=path.segments[i].end_m-path.segments[i].begin_m;
        const double trans=std::exp(-e.clouds.extinction_per_m*distance);
        for(unsigned c=0;c<3;++c) {
            const double emissive=f.asset.materials[0].emissive[c];
            require(std::abs((a[544][c]-emissive)/(b[544][c]-emissive)-trans)<=.002,"surface analytic shadow or emissive multiplied");
        }
        require(shaded.cloud_shadow_generation()==map.generation(),"surface shadow provenance missing"); save(shaded,"cloud-shadow-surface.ppm");
        f.anchor.x()=2*f.distance/f.side; sky=f.prepare(q,e);
        require(take(owner.prepare(q,e,*f.texture,s)).generation()==map.generation(),"camera movement rebuilt world shadow map");
        const auto shifted=linear(surface_shadow(f,q,sky,&map));
        for(unsigned c=0;c<3;++c) require(std::abs(shifted[16*33+15][c]-a[544][c])<=1e-5,"fixed world receiver shadow moved with camera");
    }
    f.anchor.x()=0;
    {
        auto e=vacuum(); e.clouds.enabled=true; e.clouds.coverage=.85; e.sun.direction={.6,.8,0}; auto sky=f.prepare(q,e);
        auto owner=shadow_renderer(f,q); auto real=s; real.homogeneous=false;
        auto map=take(owner.prepare(q,e,*f.texture,real)); const auto shaded=surface_shadow(f,q,sky,&map),plain=surface_shadow(f,q,sky,nullptr);
        const auto a=linear(shaded),b=linear(plain); double maximum=0;
        for(unsigned y=6;y<=26;y+=5) for(unsigned x=6;x<=26;x+=5) {
            const Vec3d point=f.anchor+ray(f,x,y)*f.distance;
            const double expected=shadow_reference(f,e,{point,Vec3d{e.sun.direction.data()},200000},real,4096);
            const auto i=y*33+x; const double observed=(a[i][0]-.01)/(b[i][0]-.01);
            maximum=std::max(maximum,std::abs(observed-expected));
        }
        std::printf("real surface bilinear versus CPU T maximum=%.9g\n",maximum); require(maximum<=.03,"real receiver shadow reference threshold");
        save(shaded,"cloud-shadow-real.ppm");
        TextureData texture; texture.id=take(AssetId::generate()); texture.width=2; texture.height=1;
        texture.rgba8={std::byte{255},std::byte{255},std::byte{255},std::byte{0},std::byte{255},std::byte{255},std::byte{255},std::byte{255}};
        texture.sampler.mag_filter=texture.sampler.min_filter=TextureFilter::nearest;
        f.asset.textures.push_back(texture); f.asset.materials[0].base_color_texture=texture.id; f.asset.materials[0].alpha_mode=AlphaMode::mask; f.upload(q);
        const auto masked=surface_shadow(f,q,sky,&map); const auto mask=linear(masked);
        require(std::abs(mask[16*33+8][0]-f.settings.clear_rgb.x())<1e-7 && std::abs(mask[16*33+24][0]-a[16*33+24][0])<1e-6,"cloud shadow changed alpha-mask clipping");
        save(masked,"cloud-shadow-mask.ppm");
        f.asset.textures.clear(); f.asset.materials[0].base_color_texture.reset(); f.asset.materials[0].alpha_mode=AlphaMode::opaque; f.upload(q);
        for(auto& energy:e.sun.irradiance) energy*=2; sky=f.prepare(q,e); auto twice=take(owner.prepare(q,e,*f.texture,real));
        const auto doubled=linear(surface_shadow(f,q,sky,&twice));
        for(unsigned c=0;c<3;++c) require(std::abs((doubled[544][c]-f.asset.materials[0].emissive[c])-2*(a[544][c]-f.asset.materials[0].emissive[c]))<=1e-5,"shadow broke solar energy scaling");
    }
    f.anchor.x()=0;
    {
        auto e=cloudy(); auto sky=f.prepare(q,e,true); auto owner=shadow_renderer(f,q); auto map=take(owner.prepare(q,e,*f.texture,s));
        const auto direct=linear(surface_shadow(f,q,sky,nullptr)),shadowed=linear(surface_shadow(f,q,sky,&map));
        f.settings.sky_diffuse=true; const auto diffuse=linear(surface_shadow(f,q,sky,nullptr)),diffuse_shadow=linear(surface_shadow(f,q,sky,&map));
        bool sky_present=false;
        for(unsigned c=0;c<3;++c) {
            sky_present|=diffuse[544][c]>direct[544][c]+1e-4;
            require(std::abs((diffuse[544][c]-diffuse_shadow[544][c])-(direct[544][c]-shadowed[544][c]))<=1e-5,"cloud shadow attenuated sky diffuse");
        }
        require(sky_present,"diffuse isolation fixture has no sky contribution"); f.settings.sky_diffuse=false;
        // Joint view-path extinction must apply the already shadowed surface exactly once.
        f.anchor.y()=2200; f.distance=2100; f.place(); sky=f.prepare(q,e,true);
        f.clouds.surface_lighting=true; f.clouds.reference.homogeneous=true;
        auto plain=f.render(q,sky); auto with=take(f.pipeline.render_clouds(q,f.view(),f.assets,sky,*f.texture,f.clouds,f.settings,&map)); require(take(with.wait(q)),"joint shadow wait");
        const auto surface=linear(surface_shadow(f,q,sky,nullptr)),surface_with=linear(surface_shadow(f,q,sky,&map));
        const auto t=diagnostic_pixels(plain,true),v=linear(plain),w=linear(with);
        for(unsigned c=0;c<3;++c) require(std::abs((v[544][c]-w[544][c])-t[544][c]*(surface[544][c]-surface_with[544][c]))<=1e-5,"joint shadow attenuation duplicated");
        CloudOptimizedSettings opt; opt.temporal=false; opt.composition=f.clouds;
        opt.composition.reference.steps=96; opt.composition.reference.sun_steps=8;
        auto optimized=take(f.pipeline.render_clouds_optimized(q,f.view(),f.assets,sky,*f.texture,opt,f.settings,&map)); require(take(optimized.wait(q)),"optimized shadow wait");
        require(optimized.cloud_shadow_generation()==map.generation(),"optimized shadow absent"); save(with,"cloud-shadow-joint.ppm");
    }
    f.anchor={0,500,0}; f.distance=400; f.place();
    for(unsigned mode=0;mode<4;++mode) {
        auto e=cloudy(); if(mode==0) e.clouds.enabled=false; if(mode==1) e.clouds.coverage=0; if(mode==2) e.clouds.density=0; if(mode==3) e.clouds.extinction_per_m=0;
        auto sky=f.prepare(q,e); auto owner=shadow_renderer(f,q); auto map=take(owner.prepare(q,e,*f.texture,s)); const auto values=shadow_pixels(map,q);
        require(std::ranges::all_of(values,[](float x){ return x==1; }),"zero cloud shadow not exactly unit");
        auto a=surface_shadow(f,q,sky,&map),b=surface_shadow(f,q,sky,nullptr);
        require(linear(a)==linear(b) && pixels(a)==pixels(b),"unit shadow does not bitwise degenerate to M14");
    }
    std::puts("shadow direct-only, sky/emissive isolation, world stability, joint attenuation and zero controls passed");
}
void shadow_failures(Fixture& f,SubmissionQueue& q,const DeviceOptions& options,memory::MemorySystem& system) {
    f.anchor={0,500,0}; f.rotation=Quatd::FromTwoVectors(Vec3d{0,0,-1},Vec3d{0,-1,0}); f.distance=400; f.place();
    auto e=cloudy(); auto sky=f.prepare(q,e); auto owner=shadow_renderer(f,q); CloudShadowSettings s; s.resolution=32; s.diagnostics=true;
    auto first=take(owner.prepare(q,e,*f.texture,s));
    std::vector<float> pending_read(std::size_t{s.resolution}*s.resolution,7);
    require(!first.read_transmittance(pending_read) && std::ranges::all_of(pending_read,[](float x){ return x==7; }),"pending shadow readback accepted or modified destination");
    const auto before=shadow_pixels(first,q); auto baseline=surface_shadow(f,q,sky,&first); const auto old_frame=linear(baseline);
    auto bad=s; bad.max_work=1; require(!owner.prepare(q,e,*f.texture,bad),"shadow work budget ignored");
    bad=s; bad.max_bytes=1; require(!owner.prepare(q,e,*f.texture,bad),"shadow byte budget ignored");
    std::array<float,1> short_read{7}; require(!first.read_transmittance(short_read) && short_read[0]==7,"invalid shadow readback wrote destination");
    {
        auto limited=shadow_renderer(f,q,{cloud_shadow_bytes(s),3}); auto only=take(limited.prepare(q,e,*f.texture,s)); shadow_pixels(only,q);
        auto next=s; next.time_seconds=1; require(!limited.prepare(q,e,*f.texture,next),"live shadow byte budget ignored");
    }
    const auto generation=owner.current().generation(); const auto count=allocations(q); s.time_seconds=1;
    fail_submit=true; require(!owner.prepare(q,e,*f.texture,s) && !fail_submit,"shadow submit failure ignored");
    require(owner.current().generation()==generation && allocations(q)==count && shadow_pixels(first,q)==before,"failed shadow candidate changed current or leaked");
    fail_submit=true; require(!f.pipeline.render_sunlight(q,f.view(),f.assets,sky,f.settings,&first) && !fail_submit,"shadow consumer submit fault ignored");
    require(allocations(q)==count && shadow_pixels(first,q)==before,"failed consumer changed shadow or leaked");
    require(linear(surface_shadow(f,q,sky,&first))==old_frame,"consumer retry differs from baseline");
    graphics::detail::ObjectAccess::fail_creation(q.resources(),VK_ERROR_OUT_OF_HOST_MEMORY);
    require(!owner.prepare(q,e,*f.texture,s) && owner.current().generation()==generation,"shadow object failure published");
    auto retry=take(owner.prepare(q,e,*f.texture,s)); require(retry.generation()==generation+1,"shadow retry generation"); shadow_pixels(retry,q);
    auto third_settings=s; third_settings.time_seconds=2; auto third=take(owner.prepare(q,e,*f.texture,third_settings)); shadow_pixels(third,q);
    third_settings.time_seconds=3; require(!owner.prepare(q,e,*f.texture,third_settings),"shadow live generation budget ignored");
    auto other=take(SubmissionQueue::create(f.heap,take(Device::create(f.heap,options))));
    require(!owner.prepare(other,e,*f.texture,s),"foreign shadow queue accepted");
    auto foreign_pipeline=take(ScenePipeline::create(f.heap,other,DK_RENDER_SHADER_DIR));
    require(!foreign_pipeline.render_sunlight(other,f.view(),f.assets,sky,f.settings,&first),"foreign shadow image accepted"); foreign_pipeline={}; check(other.close());
    CloudFieldCache mismatch_fields; auto mismatch_textures=take(CloudTextureCache::create(q));
    auto wrong=e; ++wrong.clouds.seed; auto wrong_data=take(mismatch_fields.prepare(wrong.clouds)); auto wrong_texture=take(mismatch_textures->prepare(q,*wrong_data));
    require(!owner.prepare(q,e,*wrong_texture,s),"mismatched shadow fields accepted"); wrong_texture.reset(); wrong_data.reset(); mismatch_textures.reset();
    auto changed=e; changed.clouds.coverage=.5; sky=f.prepare(q,changed);
    require(!f.pipeline.render_sunlight(q,f.view(),f.assets,sky,f.settings,&first),"stale shadow environment accepted"); sky=f.prepare(q,e);
    f.anchor.x()=10000; f.place(); sky=f.prepare(q,e); require(!f.pipeline.render_sunlight(q,f.view(),f.assets,sky,f.settings,&first),"outside shadow region silently clamped");
    f.anchor={0,2000,0}; f.distance=100; f.place(); sky=f.prepare(q,e); require(!f.pipeline.render_sunlight(q,f.view(),f.assets,sky,f.settings,&first),"inside-cloud receiver accepted");
    f.anchor={0,500,0}; f.distance=400; f.place(); sky=f.prepare(q,e);
    auto wrong_time=f.clouds; wrong_time.surface_lighting=true; wrong_time.reference.time_seconds=7;
    require(!f.pipeline.render_clouds(q,f.view(),f.assets,sky,*f.texture,wrong_time,f.settings,&first),"inconsistent cloud and shadow time accepted");
    require(linear(baseline)==old_frame && shadow_pixels(first,q)==before,"old shadow/frame changed after failures");
    baseline={}; first={}; retry={}; third={}; owner={};
    // A Frame alone retains the map's budget lease; after it drains, the slot is reclaimable.
    owner=shadow_renderer(f,q,{64ULL*1024*1024,2}); s.time_seconds=0;
    auto a=take(owner.prepare(q,e,*f.texture,s)); auto held=surface_shadow(f,q,sky,&a); a={};
    s.time_seconds=1; auto b=take(owner.prepare(q,e,*f.texture,s)); shadow_pixels(b,q); s.time_seconds=2;
    require(!owner.prepare(q,e,*f.texture,s),"frame did not retain shadow budget lease"); held={};
    auto c=take(owner.prepare(q,e,*f.texture,s)); shadow_pixels(c,q); b={}; c={}; owner={};
    auto closing=take(system.create_heap({"cloud-shadow-closing",memory::DomainCategory::render}));
    { auto renderer=take(CloudShadowRenderer::create(closing,q,std::filesystem::path{DK_RENDER_SHADER_DIR}/"../../engine/render/clouds/shaders"));
        auto pending=take(renderer.prepare(q,e,*f.texture,s)); closing.begin_close(); require(!renderer.prepare(q,e,*f.texture,s),"closed shadow owner accepted work");
        renderer={}; shadow_pixels(pending,q); }
    require(!closing.snapshot().live_allocations,"closed shadow heap leaked");
    owner=shadow_renderer(f,q); s.diagnostics=false; auto map=take(owner.prepare(q,e,*f.texture,s));
    auto pending=take(f.pipeline.render_sunlight(q,f.view(),f.assets,sky,f.settings,&map));
    owner={}; map={}; f.pipeline={}; f.assets.clear(); f.texture.reset(); f.textures.reset(); f.atmosphere={}; sky={};
    require(take(pending.wait(q)) && !linear(pending).empty(),"pending cloud shadow consumers lost owners");
    std::puts("shadow budgets, foreign/stale inputs, object/submit failures, retry, leases and pending shutdown passed");
}
