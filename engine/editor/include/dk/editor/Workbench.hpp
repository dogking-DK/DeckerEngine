#pragma once
#include <filesystem>
#include <string>

namespace dk::editor {
struct WorkbenchOptions {
    std::filesystem::path root;
    std::string pipe;
    std::filesystem::path manifest = "project.json";
    bool validation = false;
    bool disable_validation = false;
    // Bounded native UI acceptance run. Caller must supply a disposable project.
    bool smoke = false;
    bool interaction_smoke = false;
    bool simulation_smoke = false;
    bool consistency_smoke = false;
    bool simulation_response_probe = false;
    unsigned frames = 0;
    std::filesystem::path screenshot;
    bool fixture_camera = false;
};
// Main-thread window loop. Returns 77 only for an unavailable Vulkan/WSI baseline.
int run_workbench(const WorkbenchOptions&);
} // namespace dk::editor
