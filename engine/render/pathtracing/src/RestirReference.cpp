#include "RestirReferenceInternal.hpp"
#include <algorithm>
#include <cmath>

namespace dk::render {
namespace {
bool nonnegative(double x) { return std::isfinite(x) && x>=0; }
bool positive(double x) { return std::isfinite(x) && x>0; }
bool uniform_valid(double x) { return std::isfinite(x) && x>=0 && x<1; }
Error invalid() { return detail::path_invalid("Invalid RIS weight, density, support, random draw or capacity"); }
std::uint32_t mix(std::uint32_t x) noexcept {
    x^=x>>16; x*=0x7feb352du; x^=x>>15; x*=0x846ca68bu; return x^(x>>16);
}
}
Result<bool> RisReservoir::stream(double target,double pdf,double u) {
    if(!nonnegative(target) || !positive(pdf) || !uniform_valid(u) || candidates_==(1u<<24))
        return std::unexpected(invalid());
    const double w=target/pdf, sum=sum_+w;
    if(!nonnegative(w) || (target>0 && w==0) || !nonnegative(sum)) return std::unexpected(invalid());
    const bool selected=w>0 && (sum_==0 || u<w/sum);
    const double chosen_target=selected ? target : target_;
    const double normalization=sum>0 ? (sum/(candidates_+1))/chosen_target : 0;
    if(!nonnegative(normalization) || (sum>0 && normalization==0)) return std::unexpected(invalid());
    if(selected) { selected_=candidates_; target_=target; }
    ++candidates_; sum_=sum; normalization_=normalization;
    return selected;
}
Result<RisReuseSelection> resample_ris_sources(std::span<const RisReuseSource> sources,
    std::span<const std::uint8_t> support,std::span<const double> uniforms) {
    const auto count=sources.size();
    if(count>64 || support.size()!=count*count || uniforms.size()!=count) return std::unexpected(invalid());
    for(auto v:support) if(v>1) return std::unexpected(invalid());
    RisReuseSelection result;
    for(std::size_t i=0;i<count;++i) {
        const auto& s=sources[i];
        if(!nonnegative(s.target) || !nonnegative(s.weight) || !uniform_valid(uniforms[i]) ||
            s.candidates>(1u<<24) || (s.weight>0 && (!s.candidates || !support[i*count+i])) ||
            (s.weight==0 && s.target!=0)) return std::unexpected(invalid());
        result.candidates+=s.candidates;
        const double w=s.target*s.weight*s.candidates, sum=result.weight_sum+w;
        if(!nonnegative(w) || (s.target>0 && s.weight>0 && w==0) || !nonnegative(sum)) return std::unexpected(invalid());
        if(w>0 && (result.weight_sum==0 || uniforms[i]<w/sum)) result.selected=static_cast<std::uint32_t>(i);
        result.weight_sum=sum;
    }
    if(result.selected==RisReservoir::invalid_sample) return result;
    std::uint64_t denominator=0;
    for(std::size_t i=0;i<count;++i) if(support[i*count+result.selected]) denominator+=sources[i].candidates;
    if(!denominator) return std::unexpected(invalid());
    result.normalization=(result.weight_sum/double(denominator))/sources[result.selected].target;
    if(!positive(result.normalization)) return std::unexpected(invalid());
    return result;
}
double restir_reference_sample(std::uint64_t seed,std::uint32_t pixel,std::uint32_t epoch,
    std::uint32_t candidate,RestirRandomPurpose purpose) noexcept {
    auto h=mix(static_cast<std::uint32_t>(seed)^0x52495331u); // "RIS1", distinct from path v1.
    h=mix(h^static_cast<std::uint32_t>(seed>>32)); h=mix(h^pixel); h=mix(h^epoch);
    h=mix(h^candidate); h=mix(h^static_cast<std::uint32_t>(purpose));
    return (double(h>>8)+.5)/16777216.0;
}

namespace detail {
RestirLightSample sample_restir_light(const PathSceneState& scene,const PathTraceSettings& s,const Vec4d& u) {
    RestirLightSample result; result.scene_fingerprint=scene.fingerprint; result.environment_rgb=s.environment_rgb;
    const bool environment=s.environment_rgb.maxCoeff()>0, area=scene.light_weight>0;
    const int classes=int(environment)+int(area);
    if(!classes) return result;
    const double class_pdf=1.0/classes;
    if(environment && u.x()<class_pdf) {
        result.kind=RestirLightKind::environment;
        const double z=1-2*u.z(),r=std::sqrt(std::max(0.0,1-z*z)),phi=2*path_pi*u.w();
        result.coordinates={r*std::cos(phi),r*std::sin(phi),z}; result.proposal_pdf=class_pdf/(4*path_pi);
    } else {
        const double target=u.y()*scene.light_weight;
        const auto found=std::upper_bound(scene.triangles.begin(),scene.triangles.end(),target,
            [](double weight,const PathTriangle& item) { return weight<item.light_cdf; });
        if(found==scene.triangles.end()) return result;
        result.kind=RestirLightKind::triangle; result.triangle=static_cast<std::size_t>(found-scene.triangles.begin());
        const double root=std::sqrt(u.z()); result.coordinates={1-root,root*(1-u.w()),root*u.w()};
        result.proposal_pdf=class_pdf*(found->light_weight/scene.light_weight)/found->area;
    }
    return result;
}
Result<RestirLightEvaluation> evaluate_restir_light(const PathSceneState& scene,const PathTraceSettings& s,
    const PathTraceHit& hit,const PathBsdf& bsdf,const Vec3d& wo,const RestirLightSample& sample) {
    if(sample.scene_fingerprint!=scene.fingerprint || sample.environment_rgb!=s.environment_rgb)
        return std::unexpected(path_invalid("RIS light belongs to a different frozen light domain"));
    RestirLightEvaluation result; Vec3d emission=Vec3d::Zero(); double geometry=1;
    if(sample.kind==RestirLightKind::none) return result;
    if(!positive(sample.proposal_pdf) || !sample.coordinates.allFinite()) return std::unexpected(invalid());
    if(sample.kind==RestirLightKind::environment) {
        if(std::abs(sample.coordinates.squaredNorm()-1)>1e-10) return std::unexpected(invalid());
        result.direction=sample.coordinates; emission=s.environment_rgb;
    } else if(sample.kind==RestirLightKind::triangle) {
        if(sample.triangle>=scene.triangles.size() || (sample.coordinates.array()<0).any() ||
            std::abs(sample.coordinates.sum()-1)>1e-10) return std::unexpected(invalid());
        const auto& t=scene.triangles[sample.triangle];
        if(t.light_weight<=0) return std::unexpected(invalid());
        result.point=t.a*sample.coordinates.x()+t.b*sample.coordinates.y()+t.c*sample.coordinates.z();
        const Vec3d delta=result.point-hit.position; const double distance2=delta.squaredNorm();
        if(distance2==0) return result;
        result.direction=delta/std::sqrt(distance2);
        double cosine=-t.normal.dot(result.direction); if(t.double_sided) cosine=std::abs(cosine);
        geometry=std::max(0.0,cosine)/distance2; emission=t.emission;
    } else return std::unexpected(invalid());
    const auto evaluated=eval_path_bsdf(bsdf,hit.normal,wo,result.direction);
    result.contribution=evaluated.value.cwiseProduct(emission)*(std::max(0.0,hit.normal.dot(result.direction))*geometry);
    result.target=luminance(result.contribution);
    if(!result.contribution.allFinite() || (result.contribution.array()<0).any() || !nonnegative(result.target))
        return std::unexpected(invalid());
    return result;
}
Result<Vec3d> restir_direct(const PathSceneState& scene,const PathTraceHit& hit,const PathBsdf& bsdf,const Vec3d& wo,
    const PathTraceSettings& path,const RestirDiSettings& settings,std::uint32_t pixel,std::uint32_t sample,
    RestirReferenceEstimate& stats,std::stop_token cancel) {
    if(scene.light_weight==0 && path.environment_rgb.maxCoeff()==0) return Vec3d{Vec3d::Zero()};
    RisReservoir reservoir; RestirLightSample selected; RestirLightEvaluation evaluation;
    for(std::uint32_t c=0;c<settings.initial_candidates;++c) {
        if(cancel.stop_requested()) return std::unexpected(path_cancelled());
        auto random=[&](RestirRandomPurpose p) { return restir_reference_sample(path.seed,pixel,sample,c,p); };
        const auto light=sample_restir_light(scene,path,{random(RestirRandomPurpose::light_class),random(RestirRandomPurpose::light_index),
            random(RestirRandomPurpose::light_u),random(RestirRandomPurpose::light_v)});
        if(light.kind==RestirLightKind::none) return std::unexpected(Error{ErrorCode::internal_error,"RIS light CDF lost support"});
        auto candidate=evaluate_restir_light(scene,path,hit,bsdf,wo,light); if(!candidate) return std::unexpected(candidate.error());
        auto replaced=reservoir.stream(candidate->target,light.proposal_pdf,random(RestirRandomPurpose::replacement));
        if(!replaced) return std::unexpected(replaced.error());
        ++stats.candidate_evaluations;
        if(*replaced) { selected=light; evaluation=*candidate; }
    }
    if(reservoir.selected()==RisReservoir::invalid_sample) return Vec3d{Vec3d::Zero()};
    ++stats.selected_shadow_rays;
    const auto* emitter=selected.kind==RestirLightKind::triangle ? &scene.triangles[selected.triangle] : nullptr;
    if(!path_visible(scene,hit,evaluation.direction,stats.statistics,emitter,evaluation.point)) return Vec3d{Vec3d::Zero()};
    return Vec3d{evaluation.contribution*reservoir.normalization()};
}
} // namespace detail
} // namespace dk::render
