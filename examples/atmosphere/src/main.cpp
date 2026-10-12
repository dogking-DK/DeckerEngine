#include <dk/render/Atmosphere.hpp>
#include <dk/memory/MemorySystem.hpp>
#include <dk/io/File.hpp>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <stdexcept>
#include <vector>
using namespace dk;
namespace {
template<class T,class E> T take(std::expected<T,E> r) {
    if(!r) { if constexpr(std::is_same_v<E,Error>) throw std::runtime_error(r.error().message); else throw std::bad_alloc{}; }
    return std::move(*r);
}
void check(Result<void> r) { if(!r) throw std::runtime_error(r.error().message); }
struct Diagnostics { std::atomic<unsigned> errors=0,warnings=0; };
void diagnostic(void* pointer,const graphics::Diagnostic& message) noexcept {
    auto& d=*static_cast<Diagnostics*>(pointer);
    if(message.severity==VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) ++d.errors;
    if(message.severity==VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) ++d.warnings;
    if(message.severity>=VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
        std::fprintf(stderr,"%.*s: %.*s\n",int(message.name.size()),message.name.data(),int(message.message.size()),message.message.data());
}
int run(const std::filesystem::path& directory) {
    std::filesystem::create_directories(directory);
    auto system=take(memory::MemorySystem::create()); auto heap=take(system.create_heap({"atmosphere-demo",memory::DomainCategory::render}));
    Diagnostics diagnostics;
    {
        graphics::DeviceOptions options; options.validation=graphics::ValidationMode::required;
        options.diagnostic_sink=diagnostic; options.diagnostic_user_data=&diagnostics;
        auto queue=take(graphics::SubmissionQueue::create(heap,take(graphics::Device::create(heap,options))));
        auto renderer=take(render::AtmosphereRenderer::create(heap,queue,DK_ATMOSPHERE_SHADER_DIR));
        EnvironmentDescription e; e.enabled=true;
        render::SkyCamera camera; camera.width=960; camera.height=540; camera.vertical_fov_rad=1.15;
        struct Case { const char* file; double height,sun_mu,elevation,exposure; };
        for(const auto c:{Case{"noon.ppm",1,.7,.18,8},Case{"sunset.ppm",1,.015,.1,8},Case{"high-altitude.ppm",100000,.3,-.15,8}}) {
            e.sun.direction={std::sqrt(1-c.sun_mu*c.sun_mu),c.sun_mu,0};
            camera.forward={std::cos(c.elevation),std::sin(c.elevation),0};
            auto sky=take(renderer.prepare(queue,e,Vec3d{0,c.height,0}));
            auto frame=take(renderer.render_sky(queue,sky,camera));
            if(!take(frame.wait(queue))) throw std::runtime_error("sky frame wait incomplete");
            std::vector<std::array<float,4>> hdr(std::size_t{camera.width}*camera.height); check(frame.read_hdr(hdr));
            const std::string header="P6\n"+std::to_string(camera.width)+" "+std::to_string(camera.height)+"\n255\n";
            const auto bytes=std::as_bytes(std::span{header.data(),header.size()}); ByteBuffer ppm{bytes.begin(),bytes.end()};
            ppm.reserve(header.size()+hdr.size()*3); double minimum=1e30,maximum=0,mean=0;
            for(const auto& pixel:hdr) for(unsigned channel=0;channel<3;++channel) {
                const double value=pixel[channel];
                if(!std::isfinite(value) || value<0) throw std::runtime_error("sky contains invalid energy");
                minimum=std::min(minimum,value); maximum=std::max(maximum,value); mean+=value;
                const double exposed=value*c.exposure,mapped=exposed/(1+exposed);
                const double srgb=mapped<=.0031308 ? mapped*12.92 : 1.055*std::pow(mapped,1/2.4)-.055;
                ppm.push_back(std::byte(static_cast<unsigned>(std::lround(std::clamp(srgb,0.,1.)*255))));
            }
            check(write_file_bytes_atomic(directory/c.file,ppm));
            const auto g=sky.generations();
            std::printf("%s HDR min=%.9g max=%.9g mean=%.9g exposure=%.3g generations=%llu/%llu/%llu\n",c.file,minimum,maximum,mean/(hdr.size()*3),c.exposure,
                static_cast<unsigned long long>(g.transmittance),static_cast<unsigned long long>(g.multiple_scattering),static_cast<unsigned long long>(g.sky_view));
        }
        check(queue.close());
    }
    std::printf("validation errors=%u warnings=%u live=%zu\n",diagnostics.errors.load(),diagnostics.warnings.load(),heap.snapshot().live_allocations);
    return !diagnostics.errors && !diagnostics.warnings && !heap.snapshot().live_allocations && system.try_close().closed() ? 0 : 1;
}
}
#ifdef _WIN32
int wmain(int argc,wchar_t** argv) {
#else
int main(int argc,char** argv) {
#endif
    if(argc>2) { std::fprintf(stderr,"usage: dk-atmosphere-demo [OUTPUT_DIRECTORY]\n"); return 2; }
    try { return run(argc==2 ? std::filesystem::path{argv[1]} : std::filesystem::path{"atmosphere-output"}); }
    catch(const std::exception& error) { std::fprintf(stderr,"atmosphere demo: %s\n",error.what()); return 1; }
}
