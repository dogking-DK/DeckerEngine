#pragma once
#include <dk/services/CaptureService.hpp>
#include <nlohmann/json.hpp>
#include <cmath>
namespace dk::capture_detail {
inline bool valid_restir(const CaptureRestir& r) {
    if(r.version!=1 || !r.initial_candidates || r.initial_candidates>64 || !r.max_candidate_evaluations || r.max_candidate_evaluations>(1ull<<24)) return false;
    const auto receiver=[](double position,double normal) { return std::isfinite(position) && position>0 && position<=1e6 && float(position)>0 &&
        std::isfinite(normal) && normal>=-1 && normal<=1; };
    if(r.temporal) { const auto& t=*r.temporal;
        if(t.version!=1 || !t.max_history_candidates || t.max_history_candidates>4096 || !t.max_history_age || t.max_history_age>32 || (t.normal_cosine<0 || !receiver(t.position_tolerance,t.normal_cosine))) return false;
    }
    if(r.spatial) { const auto& s=*r.spatial;
        if(s.version!=1 || !s.neighbor_count || s.neighbor_count>8 || !s.radius || s.radius>32 || !s.max_source_candidates || s.max_source_candidates>4096 || !receiver(s.position_tolerance,s.normal_cosine)) return false;
    }
    return true;
}
inline nlohmann::json restir_json(const CaptureRestir& r) {
    nlohmann::json j={{"version",r.version},{"initial_candidates",r.initial_candidates},{"max_candidate_evaluations",r.max_candidate_evaluations}};
    if(r.temporal) { const auto& t=*r.temporal; j["temporal"]={{"version",t.version},{"max_history_candidates",t.max_history_candidates},
        {"max_history_age",t.max_history_age},{"position_tolerance",t.position_tolerance},{"normal_cosine",t.normal_cosine}}; }
    if(r.spatial) { const auto& s=*r.spatial; j["spatial"]={{"version",s.version},{"neighbor_count",s.neighbor_count},{"radius",s.radius},
        {"max_source_candidates",s.max_source_candidates},{"position_tolerance",s.position_tolerance},{"normal_cosine",s.normal_cosine}}; }
    return j;
}
}
