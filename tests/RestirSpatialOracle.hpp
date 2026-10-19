#pragma once
#include "../engine/render/pathtracing/src/RestirReferenceInternal.hpp"
#include <algorithm>
#include <vector>

// Double precision CPU screen-space reference. Uses independent geometry/BSDF/visibility
// and the support-matrix oracle, never SDK reservoirs or GPU packed surfaces.
namespace restir_spatial_oracle {
using namespace dk;
using namespace dk::render;
using namespace dk::render::detail;
inline std::vector<Vec3d> render(const PathTraceScene& scene,const RenderView& view,const PathTraceSettings& settings,
    const RestirDiSettings& config,unsigned epochs) {
    const auto& state=*PathReferenceAccess::get(scene); const auto& spatial=*config.spatial;
    const unsigned width=view.description().width,height=view.description().height,n=width*height;
    struct Sample {
        std::optional<PathTraceHit> hit; PathBsdf bsdf; Vec3d wo;
        RestirLightSample light; double W=0,target=0; unsigned M=0;
    };
    std::vector<Vec3d> sum(n,Vec3d::Zero());
    for(unsigned epoch=0;epoch<epochs;++epoch) {
        std::vector<Sample> input(n);
        for(unsigned pixel=0;pixel<n;++pixel) {
            auto& s=input[pixel]; const auto ray=path_trace_camera_ray(view,pixel%width,pixel/width,
                {path_trace_sample(settings.seed,pixel,epoch,0,0),path_trace_sample(settings.seed,pixel,epoch,0,1)}).value();
            s.hit=scene.nearest(ray).value(); s.wo=-ray.direction; if(!s.hit) continue;
            const auto& t=state.triangles[s.hit->index];
            s.bsdf={path_base_color(state,t,s.hit->barycentric).head<3>(),t.metallic,t.roughness,state.material_model};
            RisReservoir reservoir;
            for(unsigned i=0;i<config.initial_candidates;++i) {
                Vec4d u; for(unsigned j=0;j<4;++j) u[j]=restir_reference_sample(settings.seed,pixel,epoch,i,static_cast<RestirRandomPurpose>(j));
                const auto light=sample_restir_light(state,settings,u); if(light.kind==RestirLightKind::none) continue;
                const auto e=evaluate_restir_light(state,settings,*s.hit,s.bsdf,s.wo,light).value();
                if(reservoir.stream(e.target,light.proposal_pdf,restir_reference_sample(settings.seed,pixel,epoch,i,RestirRandomPurpose::replacement)).value()) s.light=light;
            }
            s.W=reservoir.normalization(); s.target=reservoir.target(); s.M=reservoir.candidates();
        }
        for(unsigned pixel=0;pixel<n;++pixel) {
            const auto& own=input[pixel]; if(!own.hit) { sum[pixel]+=settings.environment_rgb; continue; }
            std::vector<unsigned> sources{pixel}; std::vector<double> uniforms{0};
            const unsigned side=2*spatial.radius+1,center=side*side/2;
            for(unsigned i=0;i<spatial.neighbor_count;++i) {
                unsigned offset=std::min(unsigned(restir_reference_sample(settings.seed,pixel,epoch,i,RestirRandomPurpose::spatial_neighbor)*(side*side-1)),side*side-2);
                if(offset>=center) ++offset;
                const int x=int(pixel%width)+int(offset%side)-int(spatial.radius),y=int(pixel/width)+int(offset/side)-int(spatial.radius);
                if(x<0 || y<0 || x>=int(width) || y>=int(height)) continue;
                const unsigned index=unsigned(y)*width+unsigned(x); const auto& other=input[index];
                if(std::find(sources.begin(),sources.end(),index)!=sources.end() || !other.hit ||
                    (own.hit->position-other.hit->position).norm()>spatial.position_tolerance || own.hit->normal.dot(other.hit->normal)<spatial.normal_cosine) continue;
                sources.push_back(index); uniforms.push_back(restir_reference_sample(settings.seed,pixel,epoch,i,RestirRandomPurpose::spatial_replacement));
            }
            const auto target=[&](const Sample& receiver,const Sample& source) {
                return source.W>0 ? evaluate_restir_light(state,settings,*receiver.hit,receiver.bsdf,receiver.wo,source.light).value().target : 0;
            };
            std::vector<RisReuseSource> reuse; std::vector<std::uint8_t> support;
            for(unsigned index:sources) reuse.push_back({target(own,input[index]),input[index].W,std::min(input[index].M,spatial.max_source_candidates)});
            for(unsigned row:sources) for(unsigned column:sources) support.push_back(std::uint8_t(target(input[row],input[column])>0));
            const auto chosen=resample_ris_sources(reuse,support,uniforms).value();
            Vec3d value=state.triangles[own.hit->index].emission;
            if(chosen.selected!=RisReservoir::invalid_sample) {
                const auto& light=input[sources[chosen.selected]].light;
                const auto eval=evaluate_restir_light(state,settings,*own.hit,own.bsdf,own.wo,light).value(); PathTraceStatistics stats;
                if(path_visible(state,*own.hit,eval.direction,stats,light.kind==RestirLightKind::triangle ? &state.triangles[light.triangle] : nullptr,eval.point))
                    value+=eval.contribution*chosen.normalization;
            }
            sum[pixel]+=value;
        }
    }
    for(auto& value:sum) value/=epochs;
    return sum;
}
}
