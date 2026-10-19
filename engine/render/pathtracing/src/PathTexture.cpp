#include "PathReferenceInternal.hpp"
#include <algorithm>
#include <cmath>

namespace dk::render::detail {
namespace {
bool filter_valid(TextureFilter f) {
    switch(f) {
    case TextureFilter::nearest: case TextureFilter::linear: case TextureFilter::nearest_mipmap_nearest:
    case TextureFilter::linear_mipmap_nearest: case TextureFilter::nearest_mipmap_linear: case TextureFilter::linear_mipmap_linear: return true;
    }
    return false;
}
bool wrap_valid(TextureWrap w) { return w==TextureWrap::repeat || w==TextureWrap::clamp || w==TextureWrap::mirrored_repeat; }
int address(int x,int extent,TextureWrap wrap) {
    if(wrap==TextureWrap::clamp) return std::clamp(x,0,extent-1);
    const int period=wrap==TextureWrap::repeat ? extent : 2*extent;
    x%=period; if(x<0) x+=period;
    return x<extent ? x : 2*extent-1-x;
}
double decode(double v) { return v<=.04045 ? v/12.92 : std::pow((v+.055)/1.055,2.4); }
Vec4d texel(const PathTexture& t,int x,int y) {
    const auto index=4*(std::size_t(address(y,int(t.height),t.sampler.wrap_t))*t.width+address(x,int(t.width),t.sampler.wrap_s));
    Vec4d c;
    for(int k=0;k<4;++k) { const double v=std::to_integer<unsigned char>(t.rgba8[index+k])/255.0; c[k]=k==3 ? v : decode(v); }
    return c;
}
}
Result<void> validate_path_texture(const TextureData& t) {
    if(t.id.is_nil() || !t.width || !t.height || t.width>std::uint32_t(std::numeric_limits<int>::max()/2) || t.height>std::uint32_t(std::numeric_limits<int>::max()/2) || t.rgba8.size()!=std::uint64_t(t.width)*t.height*4 ||
       !wrap_valid(t.sampler.wrap_s) || !wrap_valid(t.sampler.wrap_t) ||
       (t.sampler.min_filter && !filter_valid(*t.sampler.min_filter)) ||
       (t.sampler.mag_filter && *t.sampler.mag_filter!=TextureFilter::nearest && *t.sampler.mag_filter!=TextureFilter::linear))
        return std::unexpected(path_invalid("Invalid path texture dimensions, RGBA8 or sampler"));
    return {};
}
Vec4d path_base_color(const PathSceneState& scene,const PathTriangle& triangle,const Vec3d& barycentric) {
    Vec4d result{triangle.albedo.x(),triangle.albedo.y(),triangle.albedo.z(),triangle.alpha};
    if(triangle.texture==0xffffffffu) return result;
    const auto& t=scene.textures[triangle.texture];
    Vec2d uv=triangle.uv_a*barycentric.x()+triangle.uv_b*barycentric.y()+triangle.uv_c*barycentric.z();
    const auto reduce=[](double x,TextureWrap wrap) {
        if(wrap==TextureWrap::clamp) return std::clamp(x,0.,1.);
        const double period=wrap==TextureWrap::repeat ? 1. : 2.;
        x=std::fmod(x,period); if(x<0) x+=period; return x>1 ? 2-x : x;
    };
    uv.x()=reduce(uv.x(),t.sampler.wrap_s); uv.y()=reduce(uv.y(),t.sampler.wrap_t);
    Vec4d color;
    if(t.sampler.mag_filter==TextureFilter::nearest) color=texel(t,int(std::floor(uv.x()*t.width)),int(std::floor(uv.y()*t.height)));
    else {
        const double x=uv.x()*t.width-.5,y=uv.y()*t.height-.5;
        const auto ix=int(std::floor(x)),iy=int(std::floor(y)); const double fx=x-ix,fy=y-iy;
        color=(texel(t,ix,iy)*(1-fx)+texel(t,ix+1,iy)*fx)*(1-fy)+(texel(t,ix,iy+1)*(1-fx)+texel(t,ix+1,iy+1)*fx)*fy;
    }
    result=result.cwiseProduct(color);
    result.w()=float(color.w())*triangle.alpha;
    return result;
}
} // namespace dk::render::detail
