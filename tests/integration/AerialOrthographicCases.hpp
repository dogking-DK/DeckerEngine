void orthographic(Fixture& f,SubmissionQueue& q) {
    EnvironmentDescription e; e.enabled=true; e.sun.direction={.3,.15,-1}; e.sun.irradiance={2,1,.5};
    const Vec3d surface=(f.asset.materials[0].base_color.head<3>()+f.asset.materials[0].emissive).cast<double>();
    struct Case { double height,span,near_m,distance,tilt; };
    for(auto c : {Case{1.8,8,.1,20,0},Case{2000,1600,500,5000,.2},Case{10000,15000,100,50000,-.1},Case{150000,10000,200,100000,-.8},Case{60000,100000,100,150000,0}}) {
        f.anchor={1234,c.height,-5678}; f.ortho_height=c.span; f.near_m=c.near_m; f.far_m=200000; f.distance=c.distance;
        f.rotation=Eigen::AngleAxisd(c.tilt,Vec3d::UnitX()); f.place(); const auto sky=f.prepare(q,e);
        f.air.reference=false; const auto actual=f.render(q,sky); f.air.reference=true; const auto high=f.render(q,sky); f.air.reference=false;
        compare(linear(actual),linear(high),"orthographic AP/sky versus GPU reference");
        const auto hdr=linear(actual);
        // Independent double world-space origin and spherical transport, including sky/ground.
        for(unsigned y : {0u,6u,16u,26u,32u}) for(unsigned x : {0u,6u,16u,26u,32u}) {
            const Vec3d origin=f.anchor+f.rotation*Vec3d{((x+.5)/33-.5)*c.span,(.5-(y+.5)/33)*c.span,-c.near_m};
            const Vec3d direction=f.rotation*Vec3d{0,0,-1}; const auto& a=f.scene->environment().atmosphere;
            const double height=take(atmosphere_height_m(a,{origin.x(),origin.y(),origin.z()}));
            const bool geometry=x>3 && x<29 && y>3 && y<29; Vec3d expected=Vec3d::Zero();
            if(height>=0) {
                const auto ref=take(integrate_atmosphere_reference(f.scene->environment(),origin,direction,{1024,1024,geometry ? c.distance-c.near_m : 2e7,false}));
                expected=ref.radiance; if(geometry && !ref.ground) expected+=ref.transmittance.cwiseProduct(surface);
            }
            const auto p=hdr[y*33+x]; const Vec3d value{p[0],p[1],p[2]};
            const double error=(value-expected).cwiseAbs().maxCoeff()/std::max(.001,expected.maxCoeff());
            require(error<=.03,"orthographic independent CPU air/sky reference");
        }
        save(actual,"aerial-orthographic-"+std::to_string(int(c.height))+".ppm");
    }
    // Isolate RGB Beer-Lambert transmission; changing near must change optical length.
    e.sun.irradiance={0,0,0}; f.anchor={0,10000,0}; f.rotation=Quatd::Identity(); f.ortho_height=12000;
    f.distance=15000; f.near_m=10; f.far_m=50000; f.place();
    f.asset.materials[0].base_color={1,1,1,1}; f.asset.materials[0].emissive={0,0,0}; f.upload(q);
    const auto sky=f.prepare(q,e); double maximum=0; std::vector<Pixel> previous;
    for(double near_m : {10.,5000.}) {
        f.near_m=near_m; const auto hdr=linear(f.render(q,sky));
        for(unsigned y=4;y<29;++y) for(unsigned x=4;x<29;++x) {
            const Vec3d origin=f.anchor+Vec3d{((x+.5)/33-.5)*f.ortho_height,(.5-(y+.5)/33)*f.ortho_height,-near_m};
            const auto ref=take(integrate_atmosphere_reference(f.scene->environment(),origin,Vec3d{0,0,-1},{1024,8,f.distance-near_m,false}));
            for(unsigned c=0;c<3;++c) maximum=std::max(maximum,std::abs(hdr[y*33+x][c]-ref.transmittance[c]));
        }
        if(!previous.empty()) require(hdr[544][0]>previous[544][0]+.001,"orthographic near plane did not skip air");
        previous=hdr;
    }
    std::printf("orthographic independent RGB T max absolute=%.9g\n",maximum); require(maximum<=.002,"orthographic AP T threshold");
    f.far_m=100000; compare(linear(f.render(q,sky)),previous,"orthographic far changes no optical length");
    const auto hdr=linear(f.render(q,sky)); f.settings.exposure=3; require(linear(f.render(q,sky))==hdr,"orthographic exposure changed physical HDR");
}
