#include "../RestirTestSupport.hpp"
#include <nlohmann/json.hpp>
#include <chrono>
#include <fstream>
#include <iostream>
#include <numbers>
#include <charconv>

using namespace restir_test;
using Json=nlohmann::json;
namespace {
// Uniform barycentric quadrature, independent of the production light/BSDF sampling.
double reference(const Fixture& f,unsigned subdivisions) {
    double sum=0;
    for(std::size_t t=2;t<f.asset.mesh.primitives.size();++t) {
        const auto& p=f.asset.mesh.primitives[t];
        const Vec3d a=p.positions[0].cast<double>(),b=p.positions[1].cast<double>(),c=p.positions[2].cast<double>();
        const double area=(b-a).cross(c-a).norm()*.5;
        double average=0;
        for(unsigned i=0;i<subdivisions;++i) for(unsigned j=0;j<subdivisions;++j) {
            const double root=std::sqrt((i+.5)/subdivisions),v=(j+.5)/subdivisions;
            const Vec3d point=a*(1-root)+b*(root*(1-v))+c*(root*v);
            const double d2=point.squaredNorm(); average+=4/(d2*d2);
        }
        sum+=area*f.asset.materials[t].emissive.x()*average/(subdivisions*subdivisions);
    }
    return double(.6f)*sum/std::numbers::pi;
}
Json rgb(const Vec3d& v) { return {v.x(),v.y(),v.z()}; }
}
int main(int argc,char** argv) try {
    unsigned count=1024,seeds=8; std::string output; bool smoke=false;
    for(int i=1;i<argc;++i) {
        const std::string arg=argv[i];
        if(arg=="--smoke") { smoke=true; count=16; seeds=1; }
        else if(arg=="--output" && i+1<argc) output=argv[++i];
        else if(arg=="--samples" && i+1<argc) {
            const std::string value=argv[++i]; const auto parsed=std::from_chars(value.data(),value.data()+value.size(),count);
            if(parsed.ec!=std::errc{} || parsed.ptr!=value.data()+value.size()) throw std::runtime_error("Invalid sample count");
        }
        else throw std::runtime_error("Expected --smoke, --samples N or --output FILE");
    }
    if(!count || count>65536) throw std::runtime_error("Samples must be 1..65536");
    Json report={{"schema",1},{"kind","cpu_initial_ris"},{"configuration",DK_BENCH_CONFIG},
        {"compiler",DK_BENCH_COMPILER},{"samples",count},{"seed_count",seeds},{"restir_version",1},
        {"initial_candidates",8},{"max_bounces",1},{"rr_start",0},{"ray_origin",{0,0,1}},
        {"ray_direction",{0,0,-1}},{"reconstruction","none"},{"history","none"},{"cases",Json::array()}};
    for(unsigned lights:{1u,100u,1000u}) {
        Fixture f; f.many_lights(lights); const auto scene=f.scene();
        const double coarse=reference(f,64),truth=reference(f,128);
        if(std::abs(truth-coarse)>.005*std::abs(truth)+1e-5) throw std::runtime_error("Reference failed convergence");
        Json item={{"lights",lights},{"scene_fingerprint",scene.fingerprint()},
            {"geometry_fingerprint",scene.geometry_fingerprint()},{"reference",truth},
            {"reference_coarse",coarse},{"reference_certified",true},{"runs",Json::array()}};
        for(unsigned seed=0;seed<seeds;++seed) for(bool ris:{false,true}) {
            auto s=settings(); s.seed=101+7919*seed;
            const auto start=std::chrono::steady_clock::now(); PathTraceEstimate estimate;
            std::uint64_t evaluations=0,shadows=0;
            if(ris) {
                const auto value=estimate_restir_reference(scene,down,s,{}, {0,count});
                if(!value) throw std::runtime_error(value.error().message);
                estimate=*value; evaluations=value->candidate_evaluations; shadows=value->selected_shadow_rays;
            } else {
                const auto value=estimate_path_reference(scene,down,s,{0,count});
                if(!value) throw std::runtime_error(value.error().message); estimate=*value;
            }
            const double ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
            if(!estimate.mean.allFinite() || !estimate.second_moment.allFinite() || ms<0 ||
                (ris && evaluations!=std::uint64_t(count)*8)) throw std::runtime_error("Invalid estimate or work accounting");
            item["runs"].push_back({{"mode",ris ? "initial_ris" : "classic_mis"},{"seed",s.seed},
                {"mean",rgb(estimate.mean)},{"second_moment",rgb(estimate.second_moment)},{"wall_ms",ms},
                {"rays",estimate.statistics.rays},{"triangle_tests",estimate.statistics.triangle_tests},
                {"candidate_evaluations",evaluations},{"selected_shadow_rays",shadows}});
        }
        report["cases"].push_back(std::move(item));
    }
    report["smoke_only"]=smoke;
    if(output.empty()) std::cout<<report.dump(2)<<'\n';
    else {
        std::ofstream file(output); file<<report.dump(2)<<'\n'; file.close();
        if(!file) throw std::runtime_error("Cannot write benchmark report");
    }
    return 0;
} catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
