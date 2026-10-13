#pragma once
// Included inside the composition probe's fixture namespace.
CloudOptimizedSettings optimized_settings() {
    CloudOptimizedSettings s; s.composition.surface_lighting=false; s.composition.sun_disk=false; s.composition.diagnostics=true;
    s.composition.reference.sky_radiance={.015,.02,.03}; s.composition.reference.time_seconds=17.25; return s;
}
void optimized_reference(Fixture& f,SubmissionQueue& q,const AtmosphereSky& sky,const CloudOptimizedSettings& settings,
    const RenderFrame& result,const char* name) {
    f.clouds=settings.composition; f.clouds.reference.steps=512; f.clouds.reference.sun_steps=f.clouds.reference.atmosphere_steps=128;
    const auto reference=f.render(q,sky); const auto actual=linear(result),expected=linear(reference);
    compare(actual,expected,name,.05,.12);
    std::vector<double> luminance; for(const auto& p:expected) luminance.push_back(.2126*p[0]+.7152*p[1]+.0722*p[2]);
    std::ranges::sort(luminance); const double scale=std::max(.001,luminance[std::size_t(.95*(luminance.size()-1))]);
    double maximum=0,transmission=0; std::size_t worst=0;
    const auto a=diagnostic_pixels(result,false),b=diagnostic_pixels(reference,false);
    for(std::size_t i=0;i<actual.size();++i) {
        for(unsigned c=0;c<3;++c) maximum=std::max(maximum,std::abs(double(actual[i][c])-expected[i][c])/scale);
        if(std::abs(double(a[i][3])-b[i][3])>transmission) { transmission=std::abs(double(a[i][3])-b[i][3]); worst=i; }
    }
    std::printf("%s all-pixel-edge=%.9g cloud_T=%.9g at %zu,%zu\n",name,maximum,transmission,worst%f.side,worst/f.side);
    if(result.cloud_optimization().history_available && transmission>.02) {
        auto cold=settings; cold.temporal=false; const auto current=diagnostic_pixels(optimized_frame(f,q,sky,cold),false);
        std::printf("failure without history: Tc=%.9g reference=%.9g temporal=%.9g\n",current[worst][3],b[worst][3],a[worst][3]);
    }
    require(maximum<=.12 && transmission<=.02,"optimized edge or cloud transmission threshold");
}
void optimization_switches(Fixture& f,SubmissionQueue& q) {
    auto settings=optimized_settings(); settings.temporal=false; settings.termination_transmittance=0;
    f.side=31; f.anchor={0,2200,0}; f.distance=3000; f.place(); auto e=cloudy(); e.clouds.coverage=.35;
    auto sky=f.prepare(q,e); auto skipped=optimized_frame(f,q,sky,settings); auto skip_stats=take(skipped.read_cloud_statistics());
    settings.empty_space_skipping=false; const auto dense=optimized_frame(f,q,sky,settings); const auto dense_stats=take(dense.read_cloud_statistics());
    compare(linear(skipped),linear(dense),"empty-space skip versus disabled",.05,.12);
    require(skip_stats.empty_steps>0 && skip_stats.primary_steps<dense_stats.primary_steps,"empty-space traversal saved no work");
    std::printf("empty-space primary %u -> %u; skipped=%u\n",dense_stats.primary_steps,skip_stats.primary_steps,skip_stats.empty_steps);
    optimized_reference(f,q,sky,settings,dense,"sparse versus reference");
    e.clouds.coverage=1; e.clouds.density=10; e.clouds.extinction_per_m=.01; e.sun.irradiance={0,0,0};
    // Uniform cloud without a geometry depth gradient isolates reconstruction work savings.
    f.asset.materials[0].alpha_mode=AlphaMode::mask; f.asset.materials[0].base_color.w()=0; f.upload(q);
    settings.composition.reference.homogeneous=true; sky=f.prepare(q,e);
    const auto complete=optimized_frame(f,q,sky,settings); const auto complete_stats=take(complete.read_cloud_statistics());
    require(complete_stats.terminated_steps==0,"disabled early termination still stopped on underflow");
    settings.termination_transmittance=1e-4f; const auto stopped=optimized_frame(f,q,sky,settings); const auto stopped_stats=take(stopped.read_cloud_statistics());
    compare(linear(stopped),linear(complete),"early termination versus complete",.05,.12);
    require(stopped_stats.reconstructed_pixels>f.side*f.side/2,"uniform cloud reconstruction reused too few pixels");
    require(stopped_stats.terminated_steps>0 && stopped_stats.primary_steps<complete_stats.primary_steps,"early termination saved no work");
    std::printf("early termination primary %u -> %u; omitted=%u; reconstructed=%u/%u\n",complete_stats.primary_steps,stopped_stats.primary_steps,stopped_stats.terminated_steps,stopped_stats.reconstructed_pixels,f.side*f.side);
    optimized_reference(f,q,sky,settings,stopped,"thick versus reference");
    // Full supported extent is a real dispatch; no linear diagnostic readback at this size.
    f.settings.linear_readback=false; settings.composition.diagnostics=false;
    auto view=f.view(); auto desc=view.description(); desc.width=1920; desc.height=1080;
    auto large=take(f.pipeline.render_clouds_optimized(q,take(RenderView::create(view.scene(),desc)),f.assets,sky,*f.texture,settings,f.settings));
    require(take(large.wait(q)) && large.info().width==1920 && large.cloud_optimization().width==960 && large.cloud_optimization().height==540,"1080p optimized dispatch");
    require(!large.read_cloud_statistics(),"unrequested statistics available");
    std::puts("1080p optimized extent completed (not a real-time performance claim)");
}
void optimization_boundaries(Fixture& f,SubmissionQueue& q) {
    auto settings=optimized_settings(); settings.temporal=false; f.side=31; f.distance=3000;
    for(unsigned mode=0;mode<4;++mode) {
        f.anchor={0,mode==0 ? 100. : mode==1 ? 6000. : 2200.,0}; f.place(); auto e=cloudy();
        if(mode==2) { e.sun.direction={0,.02,-1}; e=take(normalize_environment(e)); }
        settings.composition.surface_lighting=mode==3;
        if(mode==3) { f.distance=1000; f.place(); }
        auto sky=f.prepare(q,e,mode==3); auto frame=optimized_frame(f,q,sky,settings);
        const char* name=mode==0 ? "optimized below" : mode==1 ? "optimized above" : mode==2 ? "optimized sunset" : "optimized lit MS";
        optimized_reference(f,q,sky,settings,frame,name); save(frame,std::string{name}+".ppm");
    }
    // Uniform pure absorption has an independent analytic oracle, including the solar terminal.
    f.side=33; f.anchor={0,100,0}; f.rotation=Quatd::FromTwoVectors(Vec3d{0,0,-1},Vec3d{0,1,0}); f.distance=5000; f.place();
    auto e=cloudy(); e.sun.irradiance={0,0,0};
    e.atmosphere.rayleigh_scattering_per_m={}; e.atmosphere.mie_scattering_per_m={}; e.atmosphere.mie_extinction_per_m={};
    e.atmosphere.absorption_center_height_m=0; e.atmosphere.absorption_half_width_m=10000; e.atmosphere.absorption_extinction_per_m={1e-4,2e-4,3e-4};
    e.clouds.extinction_per_m=.0003; settings.composition.surface_lighting=false;
    settings.composition.reference.homogeneous=true; settings.composition.reference.single_scattering_albedo=0;
    auto sky=f.prepare(q,e); auto frame=optimized_frame(f,q,sky,settings); const auto medium=diagnostic_pixels(frame,true);
    const double air_depth=f.distance-(2*f.anchor.y()*f.distance+f.distance*f.distance)/20000;
    for(unsigned c=0;c<3;++c) require(std::abs(medium[544][c]-std::exp(-air_depth*e.atmosphere.absorption_extinction_per_m[c]-.75))<=.002,"optimized pure absorption T");
    require(std::abs(diagnostic_pixels(frame,false)[544][3]-std::exp(-.75))<=.002,"optimized pure cloud T");
    e=vacuum(); e.clouds.enabled=true; e.clouds.extinction_per_m=.0003; e.sun.direction={0,1,0}; e.sun.irradiance={1,1,1};
    settings.composition.sun_disk=true; f.asset.materials[0].base_color.w()=0; f.asset.materials[0].alpha_mode=AlphaMode::mask; f.upload(q);
    sky=f.prepare(q,e); frame=optimized_frame(f,q,sky,settings); const auto solar=take(solar_disk_radiance(e.sun));
    for(unsigned c=0;c<3;++c) require(std::abs(linear(frame)[544][c]/solar[c]-std::exp(-.75))<=.002,"optimized solar T");
    f.asset.materials[0].base_color.w()=1; f.asset.materials[0].alpha_mode=AlphaMode::opaque; f.upload(q);
    require(linear(optimized_frame(f,q,sky,settings))[544][0]<1,"optimized sun leaked foreground");
    settings.composition.sun_disk=false;
    for(unsigned mode=0;mode<4;++mode) {
        auto zero=cloudy(); if(mode==0) zero.clouds.enabled=false; if(mode==1) zero.clouds.coverage=0;
        if(mode==2) zero.clouds.density=0; if(mode==3) zero.clouds.extinction_per_m=0;
        sky=f.prepare(q,zero); auto old=take(f.pipeline.render_atmosphere(q,f.view(),f.assets,sky,f.settings,{false,false,false})); require(take(old.wait(q)),"old M14 wait");
        auto fallback=optimized_frame(f,q,sky,settings); require(pixels(old)==pixels(fallback) && linear(old)==linear(fallback),"optimized zero cloud was not bitwise M14");
    }
    std::puts("optimized shell viewpoints, sunset, lit MS, analytic absorption, solar and zero-cloud boundaries passed");
}
void temporal(Fixture& f,SubmissionQueue& q) {
    f.side=31; f.anchor={0,2200,0}; f.distance=3000; f.place(); auto e=cloudy(); auto sky=f.prepare(q,e);
    auto settings=optimized_settings(); std::vector<Pixel> previous; double maximum_rms=0; std::uint64_t accepted=0,rejected=0;
    RenderFrame frame;
    for(unsigned i=0;i<152;++i) {
        frame=optimized_frame(f,q,sky,settings); const auto current=linear(frame); const auto stats=take(frame.read_cloud_statistics());
        require(frame.cloud_optimization().candidate_sequence==i+1 && frame.cloud_optimization().source_sequence==i,"history sequence publication");
        accepted+=stats.history_accepted; rejected+=stats.history_rejected;
        if(i>=32) {
            double squares=0; std::vector<double> luminance;
            for(const auto& p:current) luminance.push_back(.2126*p[0]+.7152*p[1]+.0722*p[2]);
            std::ranges::sort(luminance); const double scale=std::max(.001,luminance[std::size_t(.95*(luminance.size()-1))]);
            for(std::size_t j=0;j<current.size();++j) for(unsigned c=0;c<3;++c) squares+=std::pow((current[j][c]-previous[j][c])/scale,2);
            maximum_rms=std::max(maximum_rms,std::sqrt(squares/(current.size()*3)));
        }
        previous=current;
    }
    require(accepted>0 && rejected>0 && maximum_rms<=.01,"static temporal stability or confidence gating");
    std::printf("32 warmup + 120 static frames max RMS=%.9g accepted=%llu rejected=%llu\n",maximum_rms,accepted,rejected);
    optimized_reference(f,q,sky,settings,frame,"stationary temporal reference"); save(frame,"cloud-temporal-static.ppm");
    accepted=0;
    for(unsigned i=1;i<=8;++i) {
        f.anchor.x()=i*12.; f.rotation=Quatd{Eigen::AngleAxisd{.002*i,Vec3d::UnitY()}};
        settings.composition.reference.time_seconds=17.25+i*.03; sky=f.prepare(q,e);
        frame=optimized_frame(f,q,sky,settings); require(frame.cloud_optimization().history_available,"continuous camera or wind reset history");
        accepted+=take(frame.read_cloud_statistics()).history_accepted;
        optimized_reference(f,q,sky,settings,frame,("moving wind "+std::to_string(i)).c_str());
    }
    require(accepted>0,"moving camera/wind never reused history"); save(frame,"cloud-temporal-moving.ppm");
    // A moving camera uncovers an alpha-mask boundary without changing the scene version.
    f.rotation=Quatd::Identity(); f.anchor={0,2200,0}; f.distance=1000; f.place();
    TextureData texture; texture.id=take(AssetId::generate()); texture.width=2; texture.height=1;
    texture.rgba8={std::byte{255},std::byte{255},std::byte{255},std::byte{0},std::byte{255},std::byte{255},std::byte{255},std::byte{255}};
    texture.sampler.mag_filter=texture.sampler.min_filter=TextureFilter::nearest;
    f.asset.textures.push_back(texture); f.asset.materials[0].base_color_texture=texture.id; f.asset.materials[0].alpha_mode=AlphaMode::mask; f.upload(q);
    settings.composition.reference.time_seconds=18; sky=f.prepare(q,e); frame=optimized_frame(f,q,sky,settings);
    frame=optimized_frame(f,q,sky,settings);
    for(unsigned i=1;i<=3;++i) {
        f.anchor.x()=i*80.; sky=f.prepare(q,e); frame=optimized_frame(f,q,sky,settings);
        require(frame.cloud_optimization().history_available && take(frame.read_cloud_statistics()).history_rejected>0,"disocclusion confidence not exercised");
        optimized_reference(f,q,sky,settings,frame,"mask disocclusion");
    }
    save(frame,"cloud-temporal-disocclusion.ppm");
}
void history_lifecycle(Fixture& f,SubmissionQueue& q,const DeviceOptions& options,memory::MemorySystem& system) {
    f.side=15; f.anchor={0,2200,0}; f.distance=3000; f.place(); auto e=cloudy(); auto sky=f.prepare(q,e); auto settings=optimized_settings();
    auto first=optimized_frame(f,q,sky,settings); const auto baseline=linear(first);
    auto status=take(f.pipeline.cloud_history(1)); require(status.completed==0 && status.pending==1,"unpolled candidate published history");
    check(f.pipeline.poll_cloud_history(q)); status=take(f.pipeline.cloud_history(1)); require(status.completed==1 && !status.pending && status.reusable,"completed history publication");
    auto warm=optimized_frame(f,q,sky,settings); require(warm.cloud_optimization().history_available,"warm history unavailable"); check(f.pipeline.poll_cloud_history(q));
    auto expect_reset=[&](CloudHistoryReset reason) {
        auto frame=optimized_frame(f,q,sky,settings); require(!frame.cloud_optimization().history_available && frame.cloud_optimization().reset==reason,"wrong cloud history reset reason");
        check(f.pipeline.poll_cloud_history(q)); return frame;
    };
    settings.camera_revision++; expect_reset(CloudHistoryReset::camera);
    f.side=17; expect_reset(CloudHistoryReset::resize);
    f.offset_x=.1; expect_reset(CloudHistoryReset::projection);
    settings.composition.reference.time_seconds-=1; expect_reset(CloudHistoryReset::time);
    settings.composition.reference.time_seconds+=2; expect_reset(CloudHistoryReset::time);
    settings.composition.reference.steps=128; expect_reset(CloudHistoryReset::quality);
    ++e.clouds.seed; sky=f.prepare(q,e); expect_reset(CloudHistoryReset::content);
    check(f.scene->set_name(f.entity,"edited")); expect_reset(CloudHistoryReset::scene);
    f.scene=take(SceneDocument::create()); check(f.scene->create_entity(f.entity));
    check(f.scene->set_asset_references(f.entity,{{f.asset.mesh.id,AssetKind::mesh}})); f.place(); sky=f.prepare(q,e);
    expect_reset(CloudHistoryReset::scene);
    f.anchor.x()+=501; sky=f.prepare(q,e); expect_reset(CloudHistoryReset::camera);
    f.rotation=Quatd{Eigen::AngleAxisd{.3,Vec3d::UnitY()}}; expect_reset(CloudHistoryReset::camera);
    // Exposure changes only final composition, not the medium history key.
    f.settings.exposure=2; auto exposed=optimized_frame(f,q,sky,settings); require(exposed.cloud_optimization().history_available,"exposure invalidated history");
    check(f.pipeline.poll_cloud_history(q)); f.settings.exposure=1;
    const auto sequence=take(f.pipeline.cloud_history(1)).completed; const auto resources=allocations(q);
    fail_submit=true;
    require(!f.pipeline.render_clouds_optimized(q,f.view(),f.assets,sky,*f.texture,settings,f.settings) && !fail_submit,"optimized submit fault ignored");
    status=take(f.pipeline.cloud_history(1)); require(status.completed==sequence && !status.pending && !status.reusable && allocations(q)==resources,"submit failure advanced history or leaked");
    const auto recovered=expect_reset(CloudHistoryReset::failure);
    auto cold=settings; cold.temporal=false;
    require(linear(recovered)==linear(optimized_frame(f,q,sky,cold)),"submit recovery differs from empty history");
    const auto before_object=take(f.pipeline.cloud_history(1)).completed;
    graphics::detail::ObjectAccess::fail_creation(q.resources(),VK_ERROR_OUT_OF_HOST_MEMORY);
    require(!f.pipeline.render_clouds_optimized(q,f.view(),f.assets,sky,*f.texture,settings,f.settings),"history object failure ignored");
    require(take(f.pipeline.cloud_history(1)).completed==before_object && !take(f.pipeline.cloud_history(1)).pending,"object failure advanced history");
    expect_reset(CloudHistoryReset::failure);
    auto bad=settings; bad.max_work=1; const auto valid=take(f.pipeline.cloud_history(1));
    require(!f.pipeline.render_clouds_optimized(q,f.view(),f.assets,sky,*f.texture,bad,f.settings),"optimization work budget accepted");
    require(take(f.pipeline.cloud_history(1)).completed==valid.completed,"input failure advanced history");
    auto other=take(SubmissionQueue::create(f.heap,take(Device::create(f.heap,options))));
    require(!f.pipeline.poll_cloud_history(other) && !f.pipeline.render_clouds_optimized(other,f.view(),f.assets,sky,*f.texture,settings,f.settings),"foreign history queue accepted"); check(other.close());
    for(unsigned i=2;i<=4;++i) { settings.view_id=i; auto frame=optimized_frame(f,q,sky,settings); require(frame.cloud_optimization().source_sequence==0,"cross-view history reused"); }
    settings.view_id=5; require(!f.pipeline.render_clouds_optimized(q,f.view(),f.assets,sky,*f.texture,settings,f.settings),"view slot budget ignored");
    check(f.pipeline.release_cloud_history(q,2)); auto fifth=optimized_frame(f,q,sky,settings); require(!fifth.cloud_optimization().history_available,"released view identity leaked");
    require(linear(first)==baseline,"history operations changed old frame");
    auto closing=take(system.create_heap({"cloud-history-closing",memory::DomainCategory::render}));
    { auto pipeline=take(ScenePipeline::create(closing,q,DK_RENDER_SHADER_DIR));
        auto pending=take(pipeline.render_clouds_optimized(q,f.view(),f.assets,sky,*f.texture,settings,f.settings));
        require(pending.cloud_optimization().reset==CloudHistoryReset::new_view,"new pipeline reused shader/device history"); closing.begin_close();
        require(!pipeline.render_clouds_optimized(q,f.view(),f.assets,sky,*f.texture,settings,f.settings),"closing accepted optimized frame");
        pipeline={}; require(take(pending.wait(q)) && !linear(pending).empty(),"closing lost pending optimized frame"); }
    require(!closing.snapshot().live_allocations,"closing cloud history leaked");
    settings.view_id=1; auto pending=take(f.pipeline.render_clouds_optimized(q,f.view(),f.assets,sky,*f.texture,settings,f.settings));
    f.pipeline={}; f.assets.clear(); f.texture.reset(); f.textures.reset(); f.atmosphere={}; sky={};
    require(take(pending.wait(q)) && !linear(pending).empty(),"pending history owner release failed");
    std::puts("history isolation, completion, reset, failure and pending lifetime passed");
}
