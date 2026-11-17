#pragma once
#include "PathPreview.hpp"
#include <fstream>
#include <nlohmann/json.hpp>

namespace dk::editor::detail {
inline Json media_probe_source(const render::PathTraceGpuImageInfo& info,const Camera& camera) {
    const auto world=camera.world().matrix();
    Json transform=Json::array(); for(unsigned row=0;row<4;++row) for(unsigned col=0;col<4;++col) transform.push_back(world(row,col));
    return {{"width",info.width},{"height",info.height},{"spp",info.samples.count},
        {"sampler",info.settings.sampler_version},{"seed",info.settings.seed},{"epoch_first",info.sampling_first_epoch},
        {"geometry",info.geometry_fingerprint},{"fingerprint",info.fingerprint},{"camera_revision",camera.revision()},
        {"camera_world",transform},{"restir",info.restir.has_value()}};
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
