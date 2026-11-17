// Included in the composition fixture namespace. Exercise odd sizes and both camera models.
void resolution(Fixture& f,SubmissionQueue& q) {
    auto settings=optimized_settings(); f.side=31; f.anchor={0,2200,0}; f.distance=3000;
    for(double ortho:{0.,1500.}) {
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
