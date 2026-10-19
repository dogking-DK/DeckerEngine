#include "PathReferenceInternal.hpp"
#include <Eigen/LU>
#include <algorithm>
#include <bit>
#include <cmath>

namespace dk::render {
namespace {
using namespace detail;
struct Fingerprint {
    std::uint64_t value = 14695981039346656037ull;
    void byte(std::uint8_t b) { value = (value ^ b) * 1099511628211ull; }
    void integer(std::uint64_t n) { for (unsigned i=0;i<8;++i) { byte(static_cast<std::uint8_t>(n)); n >>= 8; } }
    void number(double d) { integer(std::bit_cast<std::uint64_t>(d == 0 ? 0.0 : d)); }
    void vector(const Vec3d& v) { for (int i=0;i<3;++i) number(v[i]); }
    template<class T> void id(T id) { for (auto b:id.bytes()) byte(b); }
};
std::uint64_t input_fingerprint(const RenderScene& source,PathTraceMaterialModel model,std::uint64_t geometry) {
    Fingerprint h; h.integer(3); h.integer(3); h.integer(std::uint32_t(model));
    h.id(source.id()); h.integer(source.revision()); h.integer(geometry);
    auto vector=[&](const auto& v) { for(double x:v) h.number(x); };
    const auto e=source.environment(); const auto& a=e.atmosphere; const auto& c=e.clouds;
    h.integer(e.enabled); vector(e.sun.direction); vector(e.sun.irradiance); h.number(e.sun.angular_radius_rad);
    vector(a.planet_center_m); h.number(a.ground_radius_m); h.number(a.atmosphere_height_m);
    vector(a.rayleigh_scattering_per_m); h.number(a.rayleigh_scale_height_m);
    vector(a.mie_scattering_per_m); vector(a.mie_extinction_per_m); h.number(a.mie_scale_height_m); h.number(a.mie_anisotropy);
    vector(a.absorption_extinction_per_m); h.number(a.absorption_center_height_m); h.number(a.absorption_half_width_m); vector(a.ground_albedo);
    h.integer(c.enabled); h.integer(c.seed); h.integer(c.generator_version);
    h.integer(c.weather_resolution); h.integer(c.base_resolution); h.integer(c.detail_resolution);
    h.number(c.bottom_height_m); h.number(c.top_height_m); h.number(c.coverage); h.number(c.density); h.number(c.extinction_per_m);
    h.number(c.weather_scale_m); h.number(c.shape_scale_m); h.number(c.detail_scale_m);
    h.number(c.bottom_fade_fraction); h.number(c.top_fade_fraction); vector(c.wind_velocity_mps);
    return h.value;
}
Result<void> material_valid(const MaterialData& m, PathTraceMaterialModel model) {
    if (!m.base_color.allFinite() || (m.base_color.array()<0).any() || (m.base_color.array()>1).any() ||
        !m.emissive.allFinite() || (m.emissive.array()<0).any() || (m.emissive.array()>1e6).any() ||
        !std::isfinite(m.roughness) || m.roughness<0 || m.roughness>1 || !std::isfinite(m.metallic) || m.metallic<0 || m.metallic>1 ||
        !std::isfinite(m.alpha_cutoff) || m.alpha_cutoff<0 || m.alpha_cutoff>1)
        return std::unexpected(path_invalid("Invalid reference material"));
    if ((model==PathTraceMaterialModel::lambert && m.metallic!=0) || (m.alpha_mode!=AlphaMode::opaque && m.alpha_mode!=AlphaMode::mask))
        return std::unexpected(Error{ErrorCode::not_supported,"Path materials require opaque/mask; Lambert requires metallic=0"});
    return {};
}
}
Result<PathTraceScene> PathTraceScene::create(memory::ResourceHandle heap, const RenderScene& source,
    std::span<const CpuAsset* const> assets, const PathTraceSceneLimits& limits, PathTraceMaterialModel model) {
    if (!heap || heap.state()!=memory::ResourceState::open || !source)
        return std::unexpected(Error{ErrorCode::invalid_state,"Path scene requires an open heap and render scene"});
    constexpr auto hard=desktop_path_scene_limits();
    if ((model!=PathTraceMaterialModel::lambert && model!=PathTraceMaterialModel::metallic_roughness) || !limits.max_triangles || limits.max_triangles>hard.max_triangles ||
        !limits.max_primitive_instances || limits.max_primitive_instances>hard.max_primitive_instances || limits.max_textures>hard.max_textures || limits.max_texture_bytes>hard.max_texture_bytes)
        return std::unexpected(path_invalid("Invalid path scene limits"));
    try {
        for (std::size_t i=0;i<assets.size();++i) {
            if (!assets[i] || assets[i]->mesh.id.is_nil()) return std::unexpected(path_invalid("Invalid CPU asset identity"));
            for (std::size_t j=0;j<i;++j) if (assets[i]->mesh.id==assets[j]->mesh.id)
                return std::unexpected(path_invalid("Duplicate CPU mesh identity"));
        }
        auto state = memory::make_shared_in<detail::PathSceneState>(heap,heap);
        state->source=source; state->material_model=model;
        Fingerprint hash; hash.integer(1); hash.integer(std::uint32_t(model)); hash.id(source.id());
        std::size_t primitive_count=0, triangle_count=0, texture_bytes=0;
        for (std::size_t ei=0;ei<source.entities().size();++ei) {
            const auto& entity=source.entities()[ei];
            const auto refs=source.assets(ei); if (!refs) return std::unexpected(refs.error());
            for (const auto& ref:*refs) {
                if (ref.kind!=AssetKind::mesh) continue;
                const auto found=std::ranges::find_if(assets,[&](const CpuAsset* a) { return a->mesh.id==ref.id; });
                if (found==assets.end()) return std::unexpected(Error{ErrorCode::not_found,"Path scene mesh missing"});
                const auto& asset=**found;
                if (!entity.world.inverse()) return std::unexpected(path_invalid("Path instance transform is singular"));
                if (asset.mesh.primitives.size()>limits.max_primitive_instances-primitive_count)
                    return std::unexpected(path_invalid("Path scene primitive/material budget exceeded"));
                primitive_count+=asset.mesh.primitives.size();
                for (std::size_t i=0;i<asset.materials.size();++i) {
                    if (asset.materials[i].id.is_nil()) return std::unexpected(path_invalid("Nil material identity"));
                    for (std::size_t j=0;j<i;++j) if (asset.materials[i].id==asset.materials[j].id)
                        return std::unexpected(path_invalid("Duplicate material identity"));
                }
                hash.id(entity.id); hash.id(ref.id);
                for (std::size_t i=0;i<asset.textures.size();++i) {
                    if(asset.textures[i].id.is_nil()) return std::unexpected(path_invalid("Nil texture identity"));
                    for(std::size_t j=0;j<i;++j) if(asset.textures[i].id==asset.textures[j].id)
                        return std::unexpected(path_invalid("Duplicate texture identity"));
                }
                for (std::size_t pi=0;pi<asset.mesh.primitives.size();++pi) {
                    const auto& p=asset.mesh.primitives[pi];
                    if (p.positions.size()>3*limits.max_triangles || p.indices.size()%3 || p.indices.size()/3>limits.max_triangles-triangle_count)
                        return std::unexpected(path_invalid("Path triangle/vertex budget or topology invalid"));
                    triangle_count+=p.indices.size()/3;
                    const MaterialData defaults;
                    const MaterialData* material=&defaults;
                    if (p.material) {
                        auto m=std::ranges::find(asset.materials,*p.material,&MaterialData::id);
                        if (m==asset.materials.end()) return std::unexpected(Error{ErrorCode::not_found,"Path material missing"});
                        material=&*m;
                    }
                    if (auto valid=material_valid(*material,model);!valid) return std::unexpected(valid.error());
                    std::uint32_t texture=0xffffffffu;
                    if(material->base_color_texture) {
                        if(p.texcoords.size()!=p.positions.size()) return std::unexpected(path_invalid("Textured path primitive requires UVs"));
                        for(const auto& uv:p.texcoords) if(!uv.allFinite() || (uv.array().abs()>1e6).any())
                            return std::unexpected(path_invalid("Path UV outside finite coordinate domain"));
                        auto found_texture=std::ranges::find(asset.textures,*material->base_color_texture,&TextureData::id);
                        if(found_texture==asset.textures.end()) return std::unexpected(Error{ErrorCode::not_found,"Path texture missing"});
                        const auto prior=std::ranges::find_if(state->owned_textures,[&](const PathTexture& t) { return t.mesh==asset.mesh.id && t.id==found_texture->id; });
                        texture=std::uint32_t(prior-state->owned_textures.begin());
                        if(prior==state->owned_textures.end()) {
                            const auto& input=*found_texture;
                            if(auto valid=validate_path_texture(input);!valid) return std::unexpected(valid.error());
                            if(state->owned_textures.size()>=limits.max_textures || input.rgba8.size()>limits.max_texture_bytes-texture_bytes)
                                return std::unexpected(path_invalid("Path texture count/byte budget exceeded"));
                            state->owned_textures.emplace_back(heap); auto& target=state->owned_textures.back();
                            target.mesh=asset.mesh.id; target.id=input.id; target.width=input.width; target.height=input.height;
                            target.sampler=input.sampler; target.rgba8.assign(input.rgba8.begin(),input.rgba8.end()); target.origin=input.origin;
                            texture_bytes+=input.rgba8.size();
                            // Imported texture UUIDs can be ephemeral without a source .meta.
                            // Hash stable binding order and contents, not allocation identity.
                            hash.id(target.mesh); hash.integer(texture); hash.integer(target.width); hash.integer(target.height);
                            hash.integer(std::uint32_t(target.sampler.wrap_s)); hash.integer(std::uint32_t(target.sampler.wrap_t));
                            hash.integer(target.sampler.min_filter ? std::uint32_t(*target.sampler.min_filter) : 0);
                            hash.integer(target.sampler.mag_filter ? std::uint32_t(*target.sampler.mag_filter) : 0);
                            for(auto byte:target.rgba8) hash.byte(std::to_integer<std::uint8_t>(byte));
                            hash.integer(target.origin.size()); for(auto c:target.origin) hash.byte(std::uint8_t(c));
                        }
                    }
                    for (const auto& v:p.positions) if (!v.allFinite() || (v.array().abs()>1e6).any())
                        return std::unexpected(path_invalid("Path position outside finite coordinate domain"));
                    hash.integer(pi); hash.integer(p.indices.size());
                    for (std::size_t ti=0;ti<p.indices.size()/3;++ti) {
                        PathTriangle t;
                        Vec3d* points[]{&t.a,&t.b,&t.c};
                        Vec2d* uvs[]{&t.uv_a,&t.uv_b,&t.uv_c};
                        for (std::size_t k=0;k<3;++k) {
                            const auto index=p.indices[3*ti+k];
                            if (index>=p.positions.size()) return std::unexpected(path_invalid("Path vertex index out of range"));
                            auto world=entity.world.transform_point(p.positions[index].cast<double>());
                            if (!world || (world->array().abs()>1e6).any()) return std::unexpected(path_invalid("Path world position out of range"));
                            *points[k]=*world; hash.vector(*world);
                            if(texture!=0xffffffffu) *uvs[k]=p.texcoords[index].cast<double>();
                            hash.number(uvs[k]->x()); hash.number(uvs[k]->y());
                        }
                        t.entity=entity.id; t.mesh=ref.id; t.primitive=pi; t.triangle=ti;
                        t.albedo=material->base_color.head<3>().cast<double>(); t.emission=material->emissive.cast<double>();
                        t.double_sided=material->double_sided; t.metallic=material->metallic; t.roughness=material->roughness;
                        if(model==PathTraceMaterialModel::metallic_roughness) { hash.number(t.metallic); hash.number(t.roughness); }
                        t.texture=texture; t.alpha=material->base_color.w(); t.cutoff=material->alpha_cutoff; t.mask=material->alpha_mode==AlphaMode::mask;
                        hash.integer(texture); hash.number(t.alpha); hash.number(t.cutoff); hash.integer(t.mask);
                        hash.vector(t.albedo); hash.vector(t.emission); hash.integer(t.double_sided);
                        const Vec3d cross=(t.b-t.a).cross(t.c-t.a); const double length=cross.norm();
                        if (length==0) { ++state->degenerate; continue; }
                        t.area=length*.5; t.normal=cross/length;
                        t.light_weight=t.mask ? 0 : t.area*luminance(t.emission);
                        if(t.mask && t.emission.maxCoeff()>0) ++state->masked_emitters;
                        state->light_weight+=t.light_weight; t.light_cdf=state->light_weight;
                        state->owned_triangles.push_back(t);
                    }
                }
            }
        }
        state->triangles=state->owned_triangles; state->textures=state->owned_textures;
        state->geometry_fingerprint=hash.value;
        state->fingerprint=input_fingerprint(source,model,hash.value);
        return PathTraceScene{std::move(state)};
    } catch (const std::bad_alloc&) {
        return std::unexpected(Error{ErrorCode::internal_error,"Path scene allocation failed"});
    }
}
Result<PathTraceScene> PathTraceScene::rebind(memory::ResourceHandle heap,const RenderScene& source) const {
    if(!state_ || !heap || heap.state()!=memory::ResourceState::open || !source)
        return std::unexpected(Error{ErrorCode::invalid_state,"Rebinding requires an immutable path scene and open heap"});
    const auto changed=[] { return std::unexpected(Error{ErrorCode::conflict,"Path geometry changed; create a new scene"}); };
    const auto& old=state_->source;
    if(source.id()!=old.id() || source.entities().size()!=old.entities().size()) return changed();
    for(std::size_t i=0;i<source.entities().size();++i) {
        const auto& a=source.entities()[i]; const auto& b=old.entities()[i];
        if(a.id!=b.id || a.world.matrix()!=b.world.matrix()) return changed();
        const auto ar=source.assets(i),br=old.assets(i);
        if(!ar) return std::unexpected(ar.error());
        if(!br) return std::unexpected(br.error());
        if(!std::ranges::equal(*ar,*br)) return changed();
    }
    try {
        auto next=memory::make_shared_in<detail::PathSceneState>(heap,heap);
        next->geometry_owner=state_->geometry_owner ? state_->geometry_owner : state_;
        next->source=source; next->material_model=state_->material_model;
        next->triangles=state_->triangles; next->textures=state_->textures;
        next->light_weight=state_->light_weight; next->degenerate=state_->degenerate; next->masked_emitters=state_->masked_emitters;
        next->geometry_fingerprint=state_->geometry_fingerprint;
        next->fingerprint=input_fingerprint(source,next->material_model,next->geometry_fingerprint);
        return PathTraceScene{std::move(next)};
    } catch(const std::bad_alloc&) { return std::unexpected(Error{ErrorCode::internal_error,"Path rebind allocation failed"}); }
}
SceneId PathTraceScene::id() const noexcept { return state_ ? state_->source.id() : SceneId{}; }
std::uint64_t PathTraceScene::revision() const noexcept { return state_ ? state_->source.revision() : 0; }
std::uint64_t PathTraceScene::fingerprint() const noexcept { return state_ ? state_->fingerprint : 0; }
std::uint64_t PathTraceScene::geometry_fingerprint() const noexcept { return state_ ? state_->geometry_fingerprint : 0; }
std::size_t PathTraceScene::triangle_count() const noexcept { return state_ ? state_->triangles.size() : 0; }
std::size_t PathTraceScene::degenerate_count() const noexcept { return state_ ? state_->degenerate : 0; }
std::size_t PathTraceScene::texture_count() const noexcept { return state_ ? state_->textures.size() : 0; }
std::size_t PathTraceScene::masked_emitter_count() const noexcept { return state_ ? state_->masked_emitters : 0; }
PathTraceMaterialModel PathTraceScene::material_model() const noexcept { return state_ ? state_->material_model : PathTraceMaterialModel::lambert; }
Result<PathTraceTextureInfo> PathTraceScene::texture_info(std::size_t index) const {
    if(!state_ || index>=state_->textures.size()) return std::unexpected(path_invalid("Path texture index out of range"));
    const auto& t=state_->textures[index]; return PathTraceTextureInfo{t.mesh,t.id,t.origin,t.width,t.height};
}
Result<std::optional<PathTraceHit>> PathTraceScene::nearest(const PathTraceRay& ray) const {
    if (!state_) return std::unexpected(Error{ErrorCode::invalid_state,"Empty path scene"});
    if (auto valid=detail::validate_path_ray(ray);!valid) return std::unexpected(valid.error());
    PathTraceStatistics statistics; return detail::nearest_path(*state_,ray,statistics);
}
namespace detail {
bool same_path_geometry(const PathSceneState& a,const PathSceneState& b) {
    if(a.geometry_fingerprint!=b.geometry_fingerprint || a.source.id()!=b.source.id() || a.material_model!=b.material_model ||
        a.triangles.size()!=b.triangles.size() || a.textures.size()!=b.textures.size() || a.degenerate!=b.degenerate) return false;
    if((a.geometry_owner ? a.geometry_owner.get() : &a)==(b.geometry_owner ? b.geometry_owner.get() : &b)) return true;
    for(std::size_t i=0;i<a.triangles.size();++i) {
        const auto& x=a.triangles[i]; const auto& y=b.triangles[i];
        if(x.a!=y.a || x.b!=y.b || x.c!=y.c || x.normal!=y.normal || x.albedo!=y.albedo || x.emission!=y.emission ||
            x.uv_a!=y.uv_a || x.uv_b!=y.uv_b || x.uv_c!=y.uv_c || x.texture!=y.texture || x.alpha!=y.alpha ||
            x.cutoff!=y.cutoff || x.mask!=y.mask || x.double_sided!=y.double_sided || x.metallic!=y.metallic || x.roughness!=y.roughness ||
            x.entity!=y.entity || x.mesh!=y.mesh || x.primitive!=y.primitive || x.triangle!=y.triangle) return false;
    }
    for(std::size_t i=0;i<a.textures.size();++i) {
        const auto& x=a.textures[i]; const auto& y=b.textures[i];
        if(x.width!=y.width || x.height!=y.height || x.rgba8!=y.rgba8 || x.origin!=y.origin ||
            x.sampler.wrap_s!=y.sampler.wrap_s || x.sampler.wrap_t!=y.sampler.wrap_t ||
            x.sampler.min_filter!=y.sampler.min_filter || x.sampler.mag_filter!=y.sampler.mag_filter) return false;
    }
    return true;
}
Result<void> validate_path_ray(const PathTraceRay& ray) {
    if (!ray.origin.allFinite() || (ray.origin.array().abs()>1e6).any() || !ray.direction.allFinite() ||
        std::abs(ray.direction.squaredNorm()-1)>1e-10 || !std::isfinite(ray.minimum) || !std::isfinite(ray.maximum) ||
        ray.minimum<0 || ray.maximum<ray.minimum)
        return std::unexpected(path_invalid("Path ray requires bounded origin, unit direction and finite nonnegative interval"));
    return {};
}
std::optional<PathTraceHit> nearest_path(const PathSceneState& scene,const PathTraceRay& ray,PathTraceStatistics& stats) {
    ++stats.rays;
    std::optional<PathTraceHit> best;
    for (std::size_t i=0;i<scene.triangles.size();++i) {
        ++stats.triangle_tests;
        const auto& t=scene.triangles[i];
        const Vec3d e1=t.b-t.a,e2=t.c-t.a,p=ray.direction.cross(e2);
        const double det=e1.dot(p), scale=e1.norm()*e2.norm();
        if (std::abs(det)<=32*std::numeric_limits<double>::epsilon()*scale || (!t.double_sided && det<0)) continue;
        const Vec3d delta=ray.origin-t.a,q=delta.cross(e1);
        const double u=delta.dot(p)/det,v=ray.direction.dot(q)/det,d=e2.dot(q)/det;
        if (u<0 || v<0 || u+v>1 || d<ray.minimum || d>ray.maximum || (best && d>=best->distance)) continue;
        if(t.mask && path_base_color(scene,t,{1-u-v,u,v}).w()<t.cutoff) continue;
        PathTraceHit hit; hit.entity=t.entity; hit.mesh=t.mesh; hit.primitive=t.primitive; hit.triangle=t.triangle; hit.index=i;
        hit.distance=d; hit.barycentric={1-u-v,u,v}; hit.position=t.a*(1-u-v)+t.b*u+t.c*v;
        hit.front_face=det>0; hit.normal=hit.front_face ? t.normal : -t.normal;
        best=hit;
    }
    return best;
}
Vec3d offset_path_origin(const PathTriangle& t,const Vec3d& position,const Vec3d& direction) {
    const Vec3d extent=t.a.cwiseAbs().cwiseMax(t.b.cwiseAbs()).cwiseMax(t.c.cwiseAbs())+position.cwiseAbs();
    const double error=64*std::numeric_limits<double>::epsilon()*extent.maxCoeff();
    const Vec3d normal=direction.dot(t.normal)>=0 ? t.normal : -t.normal;
    Vec3d p=position+normal*error;
    for (int k=0;k<3;++k) if (normal[k]!=0) p[k]=std::nextafter(p[k],std::copysign(std::numeric_limits<double>::infinity(),normal[k]));
    return p;
}
} // namespace detail
} // namespace dk::render
