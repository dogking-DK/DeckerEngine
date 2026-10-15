#pragma once
#include <dk/services/CaptureService.hpp>
#include <nlohmann/json.hpp>
#include <dk/io/File.hpp>
namespace dk::capture_detail {
struct Completion { std::filesystem::path output; ByteBuffer ppm; std::string summary; nlohmann::json provenance; };
#ifdef DK_CAPTURE_PATHTRACING
Result<JobQueue::Payload> render_path_capture(memory::ResourceHandle,const SceneReadSnapshot&,const CaptureRequest&,
    const CaptureInfo&,render::ViewDescription,const std::filesystem::path&,std::stop_token,const JobQueue::Reporter&);
#endif
}
