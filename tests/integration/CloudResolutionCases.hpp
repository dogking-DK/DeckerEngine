// Included in the composition fixture namespace. Exercise odd sizes and both camera models.
void resolution(Fixture& f,SubmissionQueue& q) {
    auto settings=optimized_settings(); f.side=31; f.anchor={0,2200,0}; f.distance=3000;
    for(bool compact:{false,true}) for(double ortho:{0.,1500.}) {
        settings.compact_reconstruction=compact;
        f.ortho_height=ortho; f.place(); auto sky=f.prepare(q,cloudy());
        for(unsigned divisor:{2u,4u,2u}) {
            settings.resolution_divisor=divisor;
            auto frame=optimized_frame(f,q,sky,settings);
            require(!frame.cloud_optimization().history_available,"resolution switch reused old history");
            require(frame.cloud_optimization().width==(f.side+divisor-1)/divisor && frame.info().width==f.side,"wrong integration or output extent");
            frame=optimized_frame(f,q,sky,settings);
            require(frame.cloud_optimization().history_available,"stable resolution lost history");
            optimized_reference(f,q,sky,settings,frame,ortho ? "resolution orthographic" : "resolution perspective");
            const auto stats=take(frame.read_cloud_statistics());
            require(stats.reconstructed_pixels+stats.fallback_pixels==f.side*f.side,"reconstruction missed odd edge pixels");
            check(f.pipeline.poll_cloud_history(q));
            const auto sequence=take(f.pipeline.cloud_history(settings.view_id)).completed;
            auto changed=settings; changed.resolution_divisor=divisor==2 ? 4 : 2;
            fail_submit=true;
            require(!f.pipeline.render_clouds_optimized(q,f.view(),f.assets,sky,*f.texture,changed,f.settings),"resolution fault ignored");
            require(take(f.pipeline.cloud_history(settings.view_id)).completed==sequence,"failed resize replaced completed history");
        }
    }
    std::puts("half/quarter odd extents, projection, history and failure protection passed");
}

void compact_reconstruction(Fixture& f,SubmissionQueue& q) {
    f.side=31; f.distance=3000;
    auto modern=optimized_settings(); modern.view_id=1; modern.compact_reconstruction=true;
    auto legacy=modern; legacy.view_id=2; legacy.compact_reconstruction=false;
    for(const char* preset:{"sparse","overcast","sunset"}) for(double ortho:{0.,1500.}) for(unsigned divisor:{2u,4u}) {
        f.anchor={0,2200,0}; f.ortho_height=ortho; f.place();
        auto environment=take(environment_preset(preset)); auto sky=f.prepare(q,environment);
        modern.resolution_divisor=legacy.resolution_divisor=divisor;
        modern.composition.sun_disk=legacy.composition.sun_disk=true;
        for(unsigned i=0;i<5;++i) {
            f.anchor.x()+=i ? 4 : 0;
            sky=f.prepare(q,environment);
            modern.composition.reference.time_seconds=legacy.composition.reference.time_seconds=17.25+i/30.;
            const auto a=optimized_frame(f,q,sky,legacy),b=optimized_frame(f,q,sky,modern);
            if(i) require(a.cloud_optimization().history_available && b.cloud_optimization().history_available,"paired motion lost history");
            compare(linear(a),linear(b),"legacy versus compact reconstruction",1e-6,1e-5);
            const auto ac=diagnostic_pixels(a,false),bc=diagnostic_pixels(b,false);
            for(std::size_t j=0;j<ac.size();++j) require(std::abs(ac[j][3]-bc[j][3])<=1e-6,"compaction changed cloud transmission");
            const auto x=take(a.read_cloud_statistics()),y=take(b.read_cloud_statistics());
            require(x.fallback_pixels==y.fallback_pixels && x.reconstructed_pixels==y.reconstructed_pixels &&
                x.primary_steps==y.primary_steps && x.density_samples==y.density_samples,"compaction changed integration work");
        }
    }
    std::puts("legacy/compact paired HDR, transmission and exact work counts: 60 motion/projection/resolution cases passed");
}
