// Context-private CPU initial RIS. No SDK buffers, hero-dependent targets or visibility cache.
struct RestirLight {
    Vec3d direction=Vec3d::Zero(),point=Vec3d::Zero(),contribution=Vec3d::Zero();
    std::optional<std::size_t> triangle;
    double pdf=0;
};
struct PreparedRestir { RestirLight light; double W=0; bool surface=false; };
RestirLight restir_light(const Vertex& v,std::uint32_t pixel,std::uint32_t sample,std::uint32_t candidate) const {
    const auto u=[&](RestirRandomPurpose purpose) { return restir_reference_sample(settings.seed,pixel,sample,candidate,purpose); };
    RestirLight light; Vec3d emission; double geometry=1;
    int selected=int(u(RestirRandomPurpose::light_class)/class_pdf);
    if(environment_light && selected--==0) {
        light.direction=volume_direction(Vec3d::UnitY(),1-2*u(RestirRandomPurpose::light_u),u(RestirRandomPurpose::light_v));
        emission=settings.environment_rgb; light.pdf=class_pdf/(4*pi);
    } else if(sun_light && selected--==0) {
        light.direction=volume_direction(sun_direction,1-cone_width*u(RestirRandomPurpose::light_u),u(RestirRandomPurpose::light_v));
        emission=sun_radiance; light.pdf=class_pdf*sun_pdf;
    } else {
        const double target=u(RestirRandomPurpose::light_index)*scene.light_weight;
        const auto it=std::upper_bound(scene.triangles.begin(),scene.triangles.end(),target,[](double x,const PathTriangle& t) { return x<t.light_cdf; });
        if(it==scene.triangles.end() || it->light_weight<=0) throw path_invalid("Invalid volume RIS light proposal");
        light.triangle=std::size_t(it-scene.triangles.begin());
        const double root=std::sqrt(u(RestirRandomPurpose::light_u)),b=u(RestirRandomPurpose::light_v);
        light.point=it->a*(1-root)+it->b*(root*(1-b))+it->c*(root*b);
        light.pdf=class_pdf*(it->light_weight/scene.light_weight)/it->area;
        const Vec3d delta=light.point-v.position; const double d2=delta.squaredNorm();
        if(d2==0) return light;
        light.direction=delta/std::sqrt(d2); double cosine=-it->normal.dot(light.direction);
        if(it->double_sided) cosine=std::abs(cosine);
        geometry=std::max(0.,cosine)/d2; emission=it->emission;
    }
    light.contribution=eval_path_bsdf(v.bsdf,v.normal,-v.incoming,light.direction).value.cwiseProduct(emission)*
        (std::max(0.,v.normal.dot(light.direction))*geometry);
    if(!light.contribution.allFinite() || (light.contribution.array()<0).any()) throw path_invalid("Invalid volume RIS RGB target");
    return light;
}
PreparedRestir prepare_restir(const Vertex& v,std::uint32_t pixel,std::uint32_t sample) {
    PreparedRestir result;
    if(settings.max_bounces==0 || (!v.hit && !v.ground)) return result;
    result.surface=true;
    if(v.ground) ++restir_stats->prepared_ground; else ++restir_stats->prepared_mesh;
    if(class_pdf==0) return result;
    RisReservoir reservoir;
    for(std::uint32_t i=0;i<restir->initial_candidates;++i) {
        cancellation(); const auto light=restir_light(v,pixel,sample,i); ++restir_stats->candidate_evaluations;
        const double target=luminance(light.contribution);
        if(take(reservoir.stream(target,light.pdf,restir_reference_sample(settings.seed,pixel,sample,i,RestirRandomPurpose::replacement)))) result.light=light;
    }
    result.W=reservoir.normalization(); return result;
}
double shade_restir(const Vertex& v,const PreparedRestir& r,unsigned channel,const Random& random) {
    if(r.W==0 || r.light.contribution[channel]==0) return 0;
    const auto before=stats.rays;
    const double transmission=visibility(v,r.light.direction,r.light.triangle,r.light.point,channel,random,VolumeSampleStream::restir_shadow_path);
    restir_stats->selected_shadow_rays+=stats.rays-before;
    return r.light.contribution[channel]*r.W*transmission;
}
