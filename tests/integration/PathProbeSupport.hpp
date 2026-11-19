#pragma once
#include <dk/render/PathTraceRenderer.hpp>
#include <dk/scene/SceneDocument.hpp>
#include <cstdio>
#include <stdexcept>
#include <utility>

namespace dk::test::path_probe {
using namespace dk::graphics;
using namespace dk::render;

struct Fixture {
    memory::ResourceHandle heap;
    std::unique_ptr<SceneDocument> document=SceneDocument::create().value();
    CpuAsset asset;
    EntityId entity;
    explicit Fixture(memory::ResourceHandle h);
    void triangle(Vec3f a, Vec3f b, Vec3f c, Vec3f color, Vec3f emission={0,0,0}, bool two=false);
    void quad(float lo, float hi, float z, Vec3f color, Vec3f emission={0,0,0}, bool flip=false);
    RenderScene source() const;
    PathTraceScene scene() const;
    RenderView view(const RenderScene& source, std::uint32_t size=4) const;
};

extern bool fail_submit;
extern const PathTraceRay down;

template<class T>
T take(Result<T> result) {
    if (!result) {
        for (const auto& context : result.error().context) {
            std::fprintf(stderr,"%s\n",context.c_str());
        }
        throw std::runtime_error(result.error().message);
    }
    return std::move(*result);
}

void require(bool value,const char* message);
void check(Result<void> r);
PathTraceSettings settings(std::uint32_t depth=1);
PathTraceGpuImage complete(SubmissionQueue& q,const PathTraceFrame& frame);
PathTraceGpuImage ray_image(PathTraceRenderer& renderer,SubmissionQueue& queue,const PathTraceGpuScene& scene,PathTraceSettings s,
    std::uint32_t samples,const PathTraceRay& ray=down,std::uint32_t first=0);
void close_color(const Vec3d& value,const Vec3d& expected,double tolerance=1e-4);
void add_texture(Fixture& f,std::size_t primitive,Vec2f uv={.25f,.25f});
PathTraceScene ggx_scene(const Fixture& f);
bool collect(PathTraceRenderer& r,SubmissionQueue& q);
void advance_view(PathTraceRenderer& r,SubmissionQueue& q,std::uint64_t id);
PathTraceGpuImage finish_view(PathTraceRenderer& r,SubmissionQueue& q,std::uint64_t id);
void compare_images(const PathTraceGpuImage& a,const PathTraceGpuImage& b,double tolerance=1e-6);
Vec3d signal_rgb(const DenoisingSignalPixel::Float4& v);
void signal_recompose(const PathTraceGpuImage& image);
} // namespace dk::test::path_probe
