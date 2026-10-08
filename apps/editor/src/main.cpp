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
            else if (arg=="--validation") options.validation=true;
            else if (arg=="--smoke") options.smoke=true;
            else if (arg=="--interaction-smoke") { options.smoke=true; options.interaction_smoke=true; }
            else if (arg=="--fixture-camera") options.fixture_camera=true;
            else if (arg=="--frames") {
                const auto text=next().string(); std::size_t used=0;
                const auto count=std::stoul(text,&used);
                if (used!=text.size() || count==0 || count>10000) throw std::runtime_error("frames must be 1..10000");
                options.frames=static_cast<unsigned>(count);
            }
            else if (arg=="--screenshot") options.screenshot=next();
            else if (arg=="--help") {
                std::puts("dk-editor [--root PROJECT_ROOT] [--manifest RELATIVE_MANIFEST] [--validation]\n"
                    "Acceptance: --frames N --screenshot OUTPUT.ppm [--fixture-camera]\n"
                    "            --smoke/--interaction-smoke --root DISPOSABLE_PROJECT --screenshot OUTPUT.ppm");
                return 0;
            } else throw std::runtime_error("Unknown dk-editor option");
        }
        if (!options.screenshot.empty() && options.screenshot.extension()!=".ppm")
            throw std::runtime_error("screenshot must have .ppm extension");
        return dk::editor::run_workbench(options);
    } catch (const std::exception& error) { std::fprintf(stderr,"editor: %s\n",error.what()); return 1; }
}
