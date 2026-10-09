#include <dk/editor/Workbench.hpp>
#include <cstdio>
#include <stdexcept>
#include <string>
#ifdef _WIN32
int wmain(int argc,wchar_t** argv) {
#else
int main(int argc,char** argv) {
#endif
    try {
        dk::editor::WorkbenchOptions options;
        options.root=DK_EDITOR_DEFAULT_ROOT;
        for (int i=1;i<argc;++i) {
            const std::filesystem::path arg{argv[i]};
            const auto next=[&]() -> std::filesystem::path {
                if (++i>=argc) throw std::runtime_error("Missing option value");
                return argv[i];
            };
            if (arg=="--root") options.root=next();
            else if (arg=="--manifest") options.manifest=next();
#ifdef _WIN32
            else if (arg=="--pipe" && options.pipe.empty()) {
                const auto name=next().u8string();
                options.pipe.assign(reinterpret_cast<const char*>(name.data()),name.size());
                if (options.pipe.empty()) throw std::runtime_error("Missing pipe name");
            }
#endif
            else if (arg=="--validation") options.validation=true;
            else if (arg=="--no-validation") options.disable_validation=true;
            else if (arg=="--smoke") options.smoke=true;
            else if (arg=="--interaction-smoke") { options.smoke=true; options.interaction_smoke=true; }
            else if (arg=="--consistency-smoke") options.consistency_smoke=true;
            else if (arg=="--simulation-response-probe") options.simulation_response_probe=true;
            else if (arg=="--fixture-camera") options.fixture_camera=true;
            else if (arg=="--frames") {
                const auto text=next().string(); std::size_t used=0;
                const auto count=std::stoul(text,&used);
                if (used!=text.size() || count==0 || count>10000) throw std::runtime_error("frames must be 1..10000");
                options.frames=static_cast<unsigned>(count);
            }
            else if (arg=="--screenshot") options.screenshot=next();
            else if (arg=="--help") {
                std::puts("dk-editor [--root PROJECT_ROOT] [--manifest RELATIVE_MANIFEST] [--validation|--no-validation] [--pipe NAME]\n"
                    "Acceptance: --frames N --screenshot OUTPUT.ppm [--fixture-camera]\n"
                    "            --smoke/--interaction-smoke --root DISPOSABLE_PROJECT --screenshot OUTPUT.ppm\n"
                    "            --consistency-smoke --pipe NAME --fixture-camera --root DISPOSABLE_PROJECT --screenshot OUTPUT.ppm\n"
                    "            --simulation-response-probe --pipe NAME --root DISPOSABLE_PROJECT");
                return 0;
            } else throw std::runtime_error("Unknown dk-editor option");
        }
        if (options.validation && options.disable_validation) throw std::runtime_error("--validation and --no-validation are mutually exclusive");
        if (!options.screenshot.empty() && options.screenshot.extension()!=".ppm")
            throw std::runtime_error("screenshot must have .ppm extension");
        if (options.consistency_smoke && (options.smoke || options.frames || options.pipe.empty() || !options.fixture_camera || options.screenshot.empty()))
            throw std::runtime_error("--consistency-smoke requires --pipe, --fixture-camera and --screenshot; incompatible with --frames and other smoke modes");
        if (options.simulation_response_probe && (options.smoke || options.consistency_smoke || options.frames || options.pipe.empty()))
            throw std::runtime_error("--simulation-response-probe requires --pipe and is incompatible with smoke/frames");
        return dk::editor::run_workbench(options);
    } catch (const std::exception& error) { std::fprintf(stderr,"editor: %s\n",error.what()); return 1; }
}
