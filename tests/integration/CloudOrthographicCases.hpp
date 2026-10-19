void orthographic(Fixture& f,SubmissionQueue& q) {
    f.side=9; f.distance=6000; f.near_m=100; f.far_m=50000; f.ortho_height=1200;
    auto e=cloudy(); f.clouds.reference.time_seconds=17.25; f.clouds.reference.sky_radiance={.015,.02,.03};
    for(unsigned mode=0;mode<3;++mode) {
        f.anchor={1234,mode==0 ? 800. : mode==1 ? 2200. : 6000.,-5678};
        f.rotation=Eigen::AngleAxisd(mode==0 ? .4 : mode==1 ? .1 : -.4,Vec3d::UnitX()); f.place();
        auto sky=f.prepare(q,e); const auto reference=f.render(q,sky); cpu_image(f,reference,"orthographic independent CPU cloud image");
        auto settings=optimized_settings(); settings.temporal=false; settings.composition=f.clouds;
        const auto optimized=optimized_frame(f,q,sky,settings);
        compare(linear(optimized),linear(reference),"orthographic optimized cloud",.05,.12);
        const auto ct=diagnostic_pixels(optimized,false),rt=diagnostic_pixels(reference,false); double error=0;
        for(std::size_t i=0;i<ct.size();++i) error=std::max(error,std::abs(double(ct[i][3])-rt[i][3]));
        require(error<=.02,"orthographic optimized cloud transmission");
    }
    // Exact homogeneous cloud absorption from each distinct near-plane start.
    e=vacuum(); e.clouds.enabled=true; e.clouds.extinction_per_m=.0003; e.sun.irradiance={0,0,0};
    f.anchor={0,2200,0}; f.rotation=Quatd::Identity(); f.ortho_height=200; f.distance=1000; f.place();
    f.clouds.reference.homogeneous=true; f.clouds.reference.single_scattering_albedo=0;
    auto sky=f.prepare(q,e);
    for(double near_m : {1.,500.}) {
        f.near_m=near_m; const auto frame=f.render(q,sky); const auto transmission=diagnostic_pixels(frame,false);
        require(std::abs(transmission[40][3]-std::exp(-.0003*(1000-near_m)))<=.002,"orthographic cloud near-plane optical length");
    }
    // A near plane crossing the planet must be opaque in both full and reconstructed paths.
    f.anchor.y()=50; f.near_m=1; f.place(); sky=f.prepare(q,e);
    auto ground_settings=optimized_settings(); ground_settings.temporal=false; ground_settings.composition=f.clouds;
    for(const auto& frame : {f.render(q,sky),optimized_frame(f,q,sky,ground_settings)}) {
        const auto hdr=linear(frame);
        require(hdr[1*9+4][0]>.5 && hdr[7*9+4][0]==0,"orthographic cloud near plane inside planet leaked surface");
    }
    // Existing temporal probes exercise camera/wind motion, alpha disocclusion and static RMS with orthographic rays.
    f.ortho_height=1500; f.near_m=10; f.clouds.reference.homogeneous=false;
    temporal(f,q);
    auto settings=optimized_settings(); sky=f.prepare(q,cloudy());
    auto frame=optimized_frame(f,q,sky,settings); frame=optimized_frame(f,q,sky,settings);
    f.ortho_height*=1.2; frame=optimized_frame(f,q,sky,settings);
    require(frame.cloud_optimization().reset==CloudHistoryReset::projection,"orthographic height did not reset history");
    f.ortho_height=0; frame=optimized_frame(f,q,sky,settings);
    require(frame.cloud_optimization().reset==CloudHistoryReset::projection,"orthographic to perspective did not reset history");
    f.ortho_height=1500; frame=optimized_frame(f,q,sky,settings);
    require(frame.cloud_optimization().reset==CloudHistoryReset::projection,"perspective to orthographic did not reset history");
    const auto baseline=linear(frame); check(f.pipeline.poll_cloud_history(q)); const auto sequence=take(f.pipeline.cloud_history(1)).completed;
    fail_submit=true; require(!f.pipeline.render_clouds_optimized(q,f.view(),f.assets,sky,*f.texture,settings,f.settings),"orthographic submit fault ignored");
    require(take(f.pipeline.cloud_history(1)).completed==sequence && linear(frame)==baseline,"orthographic failed candidate changed old history/frame");
    // Newly exposed medium before the OLD near plane has no old-ray history.
    f.side=9; f.ortho_height=200; f.near_m=1000; f.distance=900; f.anchor={0,2200,0}; f.rotation=Quatd::Identity();
    f.asset.materials[0].alpha_mode=AlphaMode::opaque; f.asset.materials[0].base_color_texture.reset();
    for(auto& vertex:f.asset.mesh.primitives[0].positions) vertex*=2; f.upload(q); f.place();
    e=vacuum(); e.clouds.enabled=true; e.clouds.extinction_per_m=.1; e.clouds.top_height_m=2300;
    settings=optimized_settings(); settings.view_id=2; settings.composition.reference.homogeneous=true;
    settings.composition.reference.self_shadow=false; settings.termination_transmittance=0;
    sky=f.prepare(q,e); frame=optimized_frame(f,q,sky,settings);
    f.anchor.z()=400; sky=f.prepare(q,e); frame=optimized_frame(f,q,sky,settings);
    require(frame.cloud_optimization().history_available,"near disocclusion reset history instead of testing per-pixel rejection");
    const auto statistics=take(frame.read_cloud_statistics());
    std::printf("orthographic old-near disocclusion accepted=%u rejected=%u\n",statistics.history_accepted,statistics.history_rejected);
    require(statistics.history_accepted==0 && statistics.history_rejected==25,"orthographic old-near disocclusion reused invisible history");

}
