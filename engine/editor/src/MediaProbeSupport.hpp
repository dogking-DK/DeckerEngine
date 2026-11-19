#pragma once
#include "PathPreview.hpp"
#include <fstream>
#include <nlohmann/json.hpp>

namespace dk::editor::detail {
inline Json media_probe_source(const render::PathTraceGpuImageInfo& info,const Camera& camera) {
    const auto world=camera.world().matrix(); const auto pose=camera.pose();
    Json transform=Json::array(); for(unsigned row=0;row<4;++row) for(unsigned col=0;col<4;++col) transform.push_back(world(row,col));
    return {{"width",info.width},{"height",info.height},{"spp",info.samples.count},
        {"sampler",info.settings.sampler_version},{"seed",info.settings.seed},{"epoch_first",info.sampling_first_epoch},
        {"geometry",info.geometry_fingerprint},{"fingerprint",info.fingerprint},{"camera_revision",camera.revision()},
        {"camera_world",transform},{"restir",info.restir.has_value()},
        {"pose",{{"eye",{pose.eye.x(),pose.eye.y(),pose.eye.z()}},{"yaw",pose.yaw_deg},{"pitch",pose.pitch_deg},{"focus",pose.focus_distance}}},
        {"lens",{{"fov_y",camera.fov_y()},{"near",camera.near_plane()},{"far",camera.far_plane()}}},
        {"restir_version",info.restir ? info.restir->version : 0},{"bounces",info.settings.max_bounces},
        {"temporal_reused",info.temporal_statistics.reused},{"temporal_rejected",info.temporal_statistics.rejected},
        {"temporal_expired",info.temporal_statistics.expired},{"max_age",info.temporal_statistics.max_age},
        {"support_corrected",info.spatial_statistics.support_corrected},{"visibility_rays",info.spatial_statistics.current_visibility_rays}};
}
inline void save_media_linear(const std::filesystem::path& path,std::span<const Vec3d> pixels,Json source) {
    if(pixels.size()!=std::size_t(source.at("width").get<unsigned>())*source.at("height").get<unsigned>())
        throw std::runtime_error("Missing media probe linear pixels");
    std::ofstream file(path.string()+".rgb32f",std::ios::binary);
    for(const auto& pixel:pixels) for(unsigned c=0;c<3;++c) { const float value=float(pixel[c]); file.write(reinterpret_cast<const char*>(&value),sizeof(value)); }
    file.close(); if(!file) throw std::runtime_error("Cannot save media probe pixels");
    source["linear_file"]=path.filename().string()+".rgb32f";
    std::ofstream json(path.string()+".json"); json<<source.dump(2); json.close();
    if(!json) throw std::runtime_error("Cannot save media probe source");
}
}
