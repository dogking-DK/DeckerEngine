#include <dk/services/SimulationService.hpp>
#include <dk/io/File.hpp>
#include <dk/render/ClothRenderer.hpp>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cctype>
#include "GpuSimulation.hpp"
#include "AsyncSimulation.hpp"
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#endif

namespace dk {
namespace {
using Json = nlohmann::json;
template<class T> T take(Result<T> r) { if (!r) throw std::move(r.error()); return std::move(*r); }
void check(Result<void> r) { if (!r) throw std::move(r.error()); }
void require(bool condition, const char* message, ErrorCode code = ErrorCode::invalid_argument) {
    if (!condition) throw Error{code, message};
}
Json cloth_json(const ClothConfig& c) {
    return {{"columns",c.columns},{"rows",c.rows},{"seed",c.seed},{"spacing",c.spacing},{"height",c.height},
        {"particle_mass",c.particle_mass},{"compliance",c.compliance},{"iterations",c.physics.iterations},
        {"gravity_y",c.physics.gravity_y},{"floor_y",c.physics.floor_y},{"damping",c.physics.damping}};
}
Json metrics_json(const XpbdMetrics& m) {
    return {{"particle_count",m.particle_count},{"constraint_count",m.constraint_count},{"color_count",m.color_count},
        {"max_constraint_error",m.max_constraint_error},{"rms_constraint_error",m.rms_constraint_error},
        {"max_relative_error",m.max_relative_error},{"max_speed",m.max_speed},{"kinetic_energy",m.kinetic_energy},
        {"gravity_potential_energy",m.gravity_potential_energy},{"compliant_energy",m.compliant_energy},
        {"min_height",m.min_height},{"max_penetration",m.max_penetration},{"max_pin_displacement",m.max_pin_displacement}};
}
void write_json(const std::filesystem::path& path, const Json& json) {
    const auto text = json.dump(2) + "\n";
    check(write_file_bytes_atomic(path, std::as_bytes(std::span{text.data(),text.size()})));
}
// Refuse traversal through links/junctions. This is not a defense against concurrent hostile path replacement.
void check_components(const std::filesystem::path& root, const std::filesystem::path& relative) {
    auto path = root;
    for (const auto& part : relative) {
        require(part != ".." && part != "." && !part.empty(), "Export needs a normalized relative directory");
        auto name = take(path_to_utf8(part));
        require(name.find(':') == name.npos, "Export path cannot contain alternate streams");
        std::transform(name.begin(),name.end(),name.begin(),[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        require(name != ".decker", "Export cannot target the engine cache");
        path /= part;
        std::error_code ec;
        const auto status = std::filesystem::symlink_status(path,ec);
        require(!std::filesystem::is_symlink(status), "Export path cannot traverse symlinks");
#ifdef _WIN32
        const auto attributes = GetFileAttributesW(path.c_str());
        require(attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_REPARSE_POINT),
            "Export path cannot traverse reparse points");
#endif
    }
}
struct StagingDirectory {
    std::filesystem::path path;
    ~StagingDirectory() { if (!path.empty()) { std::error_code ec; std::filesystem::remove_all(path,ec); } }
};
}
Result<std::string> SimulationService::export_experiment(SimulationId id, std::uint64_t expected,
    std::string_view name, std::uint32_t width, std::uint32_t height) {
    auto valid = check_run(id); if (!valid) return std::unexpected(valid.error());
    try {
        require(play_->paused && !play_->fault && play_->solver.has_value(), "Export requires a paused healthy cloth simulation", ErrorCode::invalid_state);
        require(play_->clock.state().steps == expected, "Experiment steps do not match expected_steps", ErrorCode::conflict);
        require(width && height && width <= 2048 && height <= 2048, "Export dimensions must be 1..2048");
        require(!name.empty() && name.size() <= 1024, "Export output must be 1..1024 UTF-8 bytes");
        const auto relative = take(path_from_utf8(name));
        const auto output = take(play_->project.paths().resolve(relative));
        check_components(play_->project.paths().root(), relative);
        std::error_code ec;
        require(!std::filesystem::exists(output,ec) && !ec, "Experiment output already exists", ErrorCode::conflict);
        require(std::filesystem::is_directory(output.parent_path(),ec) && !ec, "Export parent must exist", ErrorCode::io_error);
        // Preallocate the result and unique sibling name before performing any external commit.
        auto result = take(path_to_utf8(output.lexically_relative(play_->project.paths().root())));
        auto temporary = output.parent_path() / (".dk-experiment-" + take(SimulationId::generate()).to_string());
        auto image = [&]() -> Result<detail::SimulationImage> {
            if (play_->task) return play_->task->capture(play_->clock.config().fixed_dt_ns,*play_->cloth,width,height);
            auto gpu = play_->gpu ? play_->gpu : take(detail::GpuSimulation::create(*play_->solver));
            return gpu->capture(play_->clock.config().fixed_dt_ns, *play_->cloth, width, height);
        }();
        if (!image) {
            if (play_->gpu && (image.error().code == ErrorCode::invalid_state || image.error().code == ErrorCode::internal_error))
                play_->fault = image.error().code;
            return std::unexpected(image.error());
        }
        auto metrics = play_->solver->evaluate(image->particles.positions, image->particles.velocities);
        if (!metrics) { if (play_->gpu) play_->fault = metrics.error().code; return std::unexpected(metrics.error()); }
        const auto run = run_state();
        const render::ClothView view;
        const Json config{{"format","DeckerSimulationExperiment"},{"version",1},
            {"solver",run.gpu ? "xpbd_gpu" : "xpbd_cpu"},{"steps",run.clock.steps},{"fixed_dt_ns",run.config.fixed_dt_ns},
            {"cloth",cloth_json(*run.cloth)},{"view",{{"width",width},{"height",height},{"view_projection",view.view_projection}}}};
        const Json report{{"steps",run.clock.steps},{"simulated_time_ns",run.clock.simulated_time_ns},{"metrics",metrics_json(*metrics)}};
        auto particles = Json::array();
        for (std::size_t i = 0; i < image->particles.positions.size(); ++i) {
            const auto& p = image->particles.positions[i]; const auto& v = image->particles.velocities[i];
            particles.push_back({{"position",{p.x,p.y,p.z}},{"velocity",{v.x,v.y,v.z}},{"inverse_mass",p.inverse_mass}});
        }
        const Json provenance{{"run_id",id.to_string()},{"document_id",run.source.document_id.to_string()},
            {"scene_id",run.source.scene_id.to_string()},{"revision",run.source.revision},
            {"files",{"config.json","metrics.json","particles.json","image.ppm"}}};
        const auto header = "P6\n" + std::to_string(width) + " " + std::to_string(height) + "\n255\n";
        ByteBuffer ppm;
        ppm.reserve(header.size() + std::size_t{width}*height*3);
        const auto bytes = std::as_bytes(std::span{header.data(),header.size()});
        ppm.insert(ppm.end(),bytes.begin(),bytes.end());
        for (std::size_t i = 0; i < image->rgba.size(); i += 4)
            ppm.insert(ppm.end(),image->rgba.begin()+i,image->rgba.begin()+i+3);
        require(std::filesystem::create_directory(temporary,ec) && !ec, "Cannot create experiment staging directory", ErrorCode::io_error);
        StagingDirectory staging{std::move(temporary)};
        write_json(staging.path / "config.json",config);
        write_json(staging.path / "metrics.json",report);
        write_json(staging.path / "particles.json",particles);
        write_json(staging.path / "provenance.json",provenance);
        check(write_file_bytes_atomic(staging.path / "image.ppm",ppm));
        require(!std::filesystem::exists(output,ec) && !ec, "Experiment output already exists", ErrorCode::conflict);
        std::filesystem::rename(staging.path,output,ec);
        require(!ec, "Cannot publish experiment directory", ErrorCode::io_error);
        staging.path.clear();
        return result;
    } catch (Error& e) { return std::unexpected(e.with_context("simulation.export")); }
}
}
