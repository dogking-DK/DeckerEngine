#pragma once
// Included in the benchmark fixture namespace. Quality never enters timing summaries.
struct QualityImage { std::vector<Pixel> rgb,cloud; };
QualityImage reference_image(Fixture& f,SubmissionQueue& q,unsigned steps,unsigned solar,bool tiles=false) {
    const auto view=f.view(); QualityImage output; output.rgb.resize(std::size_t{f.width}*f.height); output.cloud.resize(output.rgb.size(),Pixel{0,0,0,1});
    if(!f.environment.clouds.enabled) {
        auto frame=take(f.pipeline.render_atmosphere(q,view,f.assets,f.sky,f.settings)); require(take(frame.wait(q)),"clear reference wait");
        output.rgb=linear(frame); return output;
    }
    auto settings=f.optimized.composition; settings.reference.steps=steps; settings.reference.sun_steps=settings.reference.atmosphere_steps=solar;
    const unsigned tile_size=steps>512 || tiles ? 16 : f.height;
    for(unsigned y=0;y<f.height;y+=tile_size) for(unsigned x=0;x<f.width;x+=tile_size) {
        auto d=view.description(); d.height=std::min(tile_size,f.height-y); d.width=std::min(tile_size,f.width-x);
        d.projection(0,0)*=double(f.width)/d.width;
        d.projection(0,2)=(2.*x+d.width-f.width)/d.width;
        d.projection(1,1)*=double(f.height)/d.height;
        d.projection(1,2)=(2.*y+d.height-f.height)/d.height;
        auto frame=take(f.pipeline.render_clouds(q,take(RenderView::create(view.scene(),d)),f.assets,f.sky,*f.texture,settings,f.settings,&f.shadow));
        require(take(frame.wait(q)),"high reference wait");
        const auto rgb=linear(frame); std::vector<Pixel> cloud(rgb.size()); check(frame.read_cloud_rgba32f(cloud));
        for(unsigned row=0;row<d.height;++row) {
            std::copy_n(rgb.begin()+std::size_t{row}*d.width,d.width,output.rgb.begin()+std::size_t{y+row}*f.width+x);
            std::copy_n(cloud.begin()+std::size_t{row}*d.width,d.width,output.cloud.begin()+std::size_t{y+row}*f.width+x);
        }
    }
    return output;
}
double scale(const std::vector<Pixel>& reference) {
    std::vector<double> l; for(const auto& p:reference) l.push_back(.2126*p[0]+.7152*p[1]+.0722*p[2]);
    std::ranges::sort(l); return std::max(.001,l[std::size_t(.95*(l.size()-1))]);
}
Json error_metrics(const std::vector<Pixel>& actual,const std::vector<Pixel>& expected,std::span<const std::uint8_t> excluded={}) {
    const double normalization=scale(expected); double squares=0,maximum=0; std::vector<double> errors;
    require(actual.size()==expected.size() && !actual.empty(),"quality dimensions differ");
    for(std::size_t i=0;i<actual.size();++i) for(unsigned c=0;c<3;++c) {
        require(std::isfinite(actual[i][c]) && std::isfinite(expected[i][c]),"nonfinite quality image");
        if(!excluded.empty() && excluded[i]) continue;
        const double error=std::abs(double(actual[i][c])-expected[i][c])/normalization;
        squares+=error*error; maximum=std::max(maximum,error); errors.push_back(error);
    }
    std::ranges::sort(errors);
    return {{"scale",normalization},{"nrmse",std::sqrt(squares/errors.size())},{"p99",errors[std::size_t(.99*(errors.size()-1))]},{"maximum",maximum}};
}
void quality_cases(Fixture& f,SubmissionQueue& q,Json& output,const std::filesystem::path& report) {
    f.width=f.height=31; f.settings.linear_readback=true; f.optimized.composition.diagnostics=true;
    check(f.scene->set_environment(f.environment));
    output["preset"]=f.input(output["preset_name"].get<std::string>()); output["cases"]=Json::array();
    output["thresholds"]={{"nrmse",.05},{"p99",.12},{"edge_maximum",.12},{"cloud_T",.02},{"static_nrms",.01},
        {"convergence_nrmse",.02},{"convergence_p99",.04}};
    auto prepared=[&]() { f.prepare(q,f.environment); auto frame=f.render(q,f.view(),f.environment); require(take(frame.wait(q)),"quality wait"); return frame; };
    auto record=[&](const char* name,const RenderFrame& frame,bool tile_check=false) {
        const auto reference=reference_image(f,q,1024,256);
        const auto middle=reference_image(f,q,512,128);
        const auto actual=linear(frame); std::vector<std::uint8_t> solar(actual.size());
        const auto projection=f.view().description().projection; const Vec3d sun{f.environment.sun.direction.data()};
        const auto disk=take(solar_disk_radiance(f.environment.sun)); double solar_error=0,solar_convergence=0;
        for(unsigned y=0;y<f.height;++y) for(unsigned x=0;x<f.width;++x) {
            const auto i=std::size_t{y}*f.width+x;
            const Vec3d direction=(f.camera_rotation()*Vec3d{((x+.5)/f.width*2-1)/projection(0,0),((y+.5)/f.height*2-1)/projection(1,1),-1}).normalized();
            solar[i]=direction.dot(sun)>=std::cos(f.environment.sun.angular_radius_rad);
            if(solar[i]) for(unsigned c=0;c<3;++c) {
                solar_error=std::max(solar_error,std::abs(double(actual[i][c])-reference.rgb[i][c])/disk[c]);
                solar_convergence=std::max(solar_convergence,std::abs(double(middle.rgb[i][c])-reference.rgb[i][c])/disk[c]);
            }
        }
        const auto convergence=error_metrics(middle.rgb,reference.rgb,solar),quality=error_metrics(actual,reference.rgb,solar);
        double cloud_error=0;
        if(f.environment.clouds.enabled) {
            std::vector<Pixel> cloud(actual.size()); check(frame.read_cloud_rgba32f(cloud));
            for(std::size_t i=0;i<cloud.size();++i) {
                require(std::isfinite(cloud[i][3]) && std::isfinite(reference.cloud[i][3]),"nonfinite cloud transmission");
                cloud_error=std::max(cloud_error,std::abs(double(cloud[i][3])-reference.cloud[i][3]));
            }
        }
        const auto history=frame.cloud_optimization();
        Json row={{"name",name},{"eye_m",{f.eye.x(),f.eye.y(),f.eye.z()}},{"yaw_rad",f.yaw},{"time_seconds",f.time},
            {"convergence",convergence},{"quality",quality},{"cloud_T_maximum",cloud_error},
            {"unmasked_quality",error_metrics(actual,reference.rgb)},{"unmasked_convergence",error_metrics(middle.rgb,reference.rgb)},
            {"solar_pixels",std::count(solar.begin(),solar.end(),1)},{"solar_normalized_error",solar_error},{"solar_normalized_convergence",solar_convergence},
            {"history",{{"available",history.history_available},{"reset",cloud_history_reset_name(history.reset)}}},
            {"linear_rgba",actual},{"reference_linear_rgba",reference.rgb}};
        if(tile_check) {
            row["tile_equivalence"]=error_metrics(reference_image(f,q,512,128,true).rgb,middle.rgb,solar);
        }
        output["cases"].push_back(row);
        image(frame,report.parent_path()/(report.stem().string()+"-"+name+".ppm"));
        std::printf("%s %s convergence %.6g/%.6g quality %.6g/%.6g max %.6g T %.6g\n",output["preset_name"].get<std::string>().c_str(),name,
            convergence["nrmse"].get<double>(),convergence["p99"].get<double>(),quality["nrmse"].get<double>(),quality["p99"].get<double>(),quality["maximum"].get<double>(),cloud_error);
        require(convergence["nrmse"]<=.02 && convergence["p99"]<=.04,"reference not converged");
        require(solar_error<=.002 && solar_convergence<=.002,"solar transmission reference threshold");
        if(tile_check) require(row["tile_equivalence"]["nrmse"]<=.001 && row["tile_equivalence"]["p99"]<=.002,"cropped reference differs from full projection");
        require(quality["nrmse"]<=.05 && quality["p99"]<=.12 && quality["maximum"]<=.12 && cloud_error<=.02,"cloud quality or edge threshold");
    };
    f.reset(q); std::vector<Pixel> previous; double maximum_nrms=0; RenderFrame frame;
    for(unsigned i=0;i<152;++i) {
        frame=prepared(); const auto current=linear(frame);
        if(i>=32) maximum_nrms=std::max(maximum_nrms,error_metrics(current,previous)["nrmse"].get<double>());
        previous=current;
    }
    output["static_stability"]={{"warmup",32},{"samples",120},{"maximum_nrms",maximum_nrms}};
    require(maximum_nrms<=.01,"static frame variation exceeded threshold"); record("static",frame,true);
    // Each dynamic sample is compared at exactly the same camera and wind time.
    for(unsigned i=1;i<=12;++i) {
        f.eye.x()=i*8.; f.yaw=i*.001; f.time=i/30.; frame=prepared();
        if(f.environment.clouds.enabled) require(frame.cloud_optimization().history_available,"continuous motion lost history");
        const auto name="motion-"+std::to_string(i); record(name.c_str(),frame);
    }
    if(f.environment.clouds.enabled) {
        for(double height:{2200.,5000.}) {
            f.reset(q); f.eye={0,height,0}; f.yaw=0; f.time=0;
            for(unsigned i=0;i<33;++i) frame=prepared();
            record(height==2200 ? "inside" : "above",frame);
        }
        // Same time rewind and explicit camera cut must reject completed history.
        f.time=.1; frame=prepared(); f.time=0; frame=prepared();
        require(!frame.cloud_optimization().history_available && frame.cloud_optimization().reset==CloudHistoryReset::time,"rewind history was reused");
        ++f.optimized.camera_revision; frame=prepared();
        require(!frame.cloud_optimization().history_available && frame.cloud_optimization().reset==CloudHistoryReset::camera,"camera cut reused history");
        output["reset_checks"]={"time","camera"};
    }
}
