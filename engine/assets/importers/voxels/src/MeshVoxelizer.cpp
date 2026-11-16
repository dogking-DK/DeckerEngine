#include <dk/assets/MeshVoxelizer.hpp>
#include <openvdb/openvdb.h>
#include <openvdb/io/Stream.h>
#include <openvdb/tools/MeshToVolume.h>
#include <openvdb/math/Proximity.h>
#include <nlohmann/json.hpp>
#include <chrono>
#include <bit>
#include <cmath>
#include <map>
#include <sstream>

namespace dk::assets {
namespace {
using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;
void require(bool ok, const char* why, ErrorCode code = ErrorCode::invalid_argument) { if (!ok) throw Error{code, why}; }
double milliseconds(Clock::time_point start) { return std::chrono::duration<double, std::milli>(Clock::now()-start).count(); }
struct Triangle { const MeshPrimitive* primitive; std::size_t first; std::size_t material; };
struct Adapter {
    const std::vector<Triangle>& triangles;
    const openvdb::math::Transform& transform;
    std::size_t points;
    std::size_t polygonCount() const { return triangles.size(); }
    std::size_t pointCount() const { return points; }
    std::size_t vertexCount(std::size_t) const { return 3; }
    void getIndexSpacePoint(std::size_t n, std::size_t v, openvdb::Vec3d& p) const {
        const auto& t = triangles[n]; const auto& a = t.primitive->positions[t.primitive->indices[t.first+v]];
        p = transform.worldToIndex(openvdb::Vec3d(a.x(), a.y(), a.z()));
    }
};
struct Interrupt {
    std::stop_token stop;
    void start(const char* = nullptr) {}
    void end() {}
    bool wasInterrupted(int = -1) { return stop.stop_requested(); }
};
int wrap(int x, int size, TextureWrap mode) {
    if (mode == TextureWrap::clamp) return std::clamp(x, 0, size-1);
    const int period = mode == TextureWrap::mirrored_repeat ? size*2 : size;
    x = ((x % period)+period)%period;
    return x < size ? x : period-x-1;
}
float linear(float v) { return v <= .04045f ? v/12.92f : std::pow((v+.055f)/1.055f, 2.4f); }
Vec4f sample(const TextureData& t, const Vec2d& uv) {
    // Reduce before integer conversion, including very large finite UVs.
    const auto coordinate=[](double u, TextureWrap mode) {
        if (mode == TextureWrap::clamp) return std::clamp(u, 0.0, 1.0);
        const double period=mode == TextureWrap::mirrored_repeat ? 2.0 : 1.0;
        return u-std::floor(u/period)*period;
    };
    const double x=coordinate(uv.x(),t.sampler.wrap_s)*t.width-.5, y=coordinate(uv.y(),t.sampler.wrap_t)*t.height-.5;
    const int ix=int(std::floor(x)), iy=int(std::floor(y));
    const auto pixel=[&](int a,int b) {
        const auto offset=(std::size_t(wrap(b,int(t.height),t.sampler.wrap_t))*t.width+wrap(a,int(t.width),t.sampler.wrap_s))*4;
        Vec4f c; for(int k=0;k<4;++k) { c[k]=std::to_integer<unsigned char>(t.rgba8[offset+k])/255.f; if(k<3) c[k]=linear(c[k]); }
        return c;
    };
    const float fx=float(x-ix), fy=float(y-iy);
    return ((1-fy)*((1-fx)*pixel(ix,iy)+fx*pixel(ix+1,iy))+fy*((1-fx)*pixel(ix,iy+1)+fx*pixel(ix+1,iy+1))).eval();
}
}
Result<MeshVoxelization> voxelize_mesh(const CpuAsset& source, const MeshVoxelizeOptions& options) {
    try {
        const auto start=Clock::now();
        require(!source.voxel && options.resolution>=16 && options.resolution<=512 && options.maximum_candidates>0 && options.maximum_tree_bytes>0, "Invalid mesh voxelization options");
        const auto checkpoint=[&] { require(!options.stop.stop_requested(), "Mesh voxelization cancelled", ErrorCode::invalid_state); }; checkpoint();
        std::vector<Triangle> triangles;
        Vec3d minimum=Vec3d::Constant(std::numeric_limits<double>::max()), maximum=-minimum;
        std::size_t points=0;
        std::vector<MaterialData> materials(source.materials.begin(),source.materials.end());
        materials.emplace_back(); materials.back().metallic=0;
        std::vector<const TextureData*> textures(materials.size(),nullptr);
        for(std::size_t i=0;i<materials.size();++i) {
            const auto& m=materials[i];
            require(m.alpha_mode!=AlphaMode::blend,"Transparent BLEND material cannot become opaque voxels",ErrorCode::not_supported);
            require(m.base_color.allFinite() && (m.base_color.array()>=0).all() && (m.base_color.array()<=1).all() &&
                std::isfinite(m.metallic) && m.metallic>=0 && m.metallic<=1 && std::isfinite(m.roughness) && m.roughness>=0 && m.roughness<=1 &&
                std::isfinite(m.alpha_cutoff) && m.alpha_cutoff>=0,"Invalid voxel material");
            if(m.base_color_texture) {
                for(const auto& t:source.textures) if(t.id==*m.base_color_texture) textures[i]=&t;
                require(textures[i]!=nullptr,"Missing base color texture"); const auto& t=*textures[i];
                require(t.width>0 && t.height>0 && t.width<=8192 && t.height<=8192 && t.rgba8.size()==std::uint64_t(t.width)*t.height*4,"Invalid texture dimensions");
            }
        }
        for(const auto& p:source.mesh.primitives) {
            require(p.indices.size()%3==0 && p.positions.size()<=1000000 && points<=1000000-p.positions.size(),"Invalid mesh size"); points+=p.positions.size();
            std::size_t material=materials.size()-1;
            if(p.material) { for(std::size_t i=0;i<source.materials.size();++i) if(materials[i].id==*p.material) material=i; require(material!=materials.size()-1,"Missing mesh material"); }
            if(textures[material]) require(p.texcoords.size()==p.positions.size(),"Textured voxelization requires UV0");
            for(const auto& uv:p.texcoords) require(uv.allFinite(),"Invalid mesh UV");
            for(auto i:p.indices) {
                require(i<p.positions.size() && p.positions[i].allFinite(),"Invalid mesh index/position");
                minimum=minimum.cwiseMin(p.positions[i].cast<double>()); maximum=maximum.cwiseMax(p.positions[i].cast<double>());
            }
            require(p.indices.size()/3<=1000000-triangles.size(),"Too many voxelization triangles");
            for(std::size_t i=0;i<p.indices.size();i+=3) triangles.push_back({&p,i,material});
        }
        const double extent=(maximum-minimum).maxCoeff();
        require(!triangles.empty() && std::isfinite(extent) && extent>0,"Mesh has no finite extent");
        const double voxel=extent/options.resolution;
        require(std::isfinite(voxel) && voxel>1e-12 && minimum.cwiseAbs().maxCoeff()/voxel<1e12,"Mesh scale cannot be represented reliably");
        openvdb::initialize();
        auto transform=openvdb::math::Transform::createLinearTransform(voxel);
        transform->postTranslate(openvdb::Vec3d(minimum.x(),minimum.y(),minimum.z())+openvdb::Vec3d(voxel*.5));
        auto indices=openvdb::Int32Grid::create(-1);
        Adapter adapter{triangles,*transform,points}; Interrupt interrupt{options.stop};
        const auto distance_start=Clock::now();
        auto distance=openvdb::tools::meshToVolume<openvdb::FloatGrid>(interrupt,adapter,*transform,1.f,1.f,
            openvdb::tools::UNSIGNED_DISTANCE_FIELD|openvdb::tools::DISABLE_RENORMALIZATION,indices.get());
        checkpoint(); require(bool(distance),"OpenVDB conversion failed",ErrorCode::internal_error);
        const double distance_ms=milliseconds(distance_start);
        require(distance->activeVoxelCount()<=options.maximum_candidates,"Voxelization candidate budget exceeded");
        const auto temporary_bytes=distance->memUsage()+indices->memUsage();
        require(temporary_bytes<=options.maximum_tree_bytes,"Voxelization tree budget exceeded");
        auto solid=openvdb::Int32Grid::create(0); solid->setName("solid"); solid->setTransform(transform);
        auto access=solid->getAccessor(); auto polygon=indices->getConstAccessor();
        std::map<std::array<unsigned,5>,int> palette_ids; Json palette=Json::array();
        const auto bake_start=Clock::now();
        std::uint64_t visited=0,masked=0;
        for(auto it=distance->cbeginValueOn();it;++it) {
            if((++visited%4096)==0) checkpoint();
            require(it.isVoxelValue(),"Unexpected distance tile",ErrorCode::internal_error);
            if(*it>voxel*std::sqrt(3.0)*.5) continue;
            const auto coord=it.getCoord(); const int index=polygon.getValue(coord);
            require(index>=0 && std::size_t(index)<triangles.size(),"Missing closest polygon",ErrorCode::internal_error);
            const auto& tri=triangles[index]; const auto& p=*tri.primitive; const auto& m=materials[tri.material];
            Vec4f color=m.base_color;
            if(const auto* texture=textures[tri.material];texture && (options.texture_colors || m.alpha_mode==AlphaMode::mask)) {
                openvdb::Vec3d a,b,c,weights; adapter.getIndexSpacePoint(index,0,a); adapter.getIndexSpacePoint(index,1,b); adapter.getIndexSpacePoint(index,2,c);
                openvdb::math::closestPointOnTriangleToPoint(a,b,c,openvdb::Vec3d(coord.x(),coord.y(),coord.z()),weights);
                Vec2d uv=Vec2d::Zero(); for(int v=0;v<3;++v) uv+=p.texcoords[p.indices[tri.first+v]].cast<double>()*weights[v];
                require(uv.allFinite(),"Degenerate triangle UV interpolation"); const auto texel=sample(*texture,uv);
                if(options.texture_colors) color.head<3>()=color.head<3>().cwiseProduct(texel.head<3>());
                color.w()*=texel.w();
            }
            if(m.alpha_mode==AlphaMode::mask && color.w()<m.alpha_cutoff) { ++masked; continue; }
            std::array<unsigned,5> key{std::bit_cast<unsigned>(m.metallic),std::bit_cast<unsigned>(m.roughness),unsigned(std::lround(color.x()*15)),unsigned(std::lround(color.y()*15)),unsigned(std::lround(color.z()*15))};
            auto [entry,fresh]=palette_ids.try_emplace(key,int(palette.size()+1));
            if(fresh) { require(palette.size()<8192,"Voxel palette exceeds 8192 colors; use material colors or lower resolution"); palette.push_back({{"base_color",{key[2]/15.0,key[3]/15.0,key[4]/15.0}},{"metallic",m.metallic},{"roughness",m.roughness}}); }
            access.setValue(coord,entry->second);
        }
        checkpoint(); require(solid->activeVoxelCount()>0,"Mesh produced no occupied voxels"); solid->tree().prune();
        require(solid->memUsage()<=options.maximum_tree_bytes,"Solid tree budget exceeded");
        const auto bake_ms=milliseconds(bake_start); const auto bbox=solid->evalActiveVoxelBoundingBox();
        Json descriptor={{"format","DeckerVoxelSource"},{"version",1},{"vdb","grid.vdb"},{"grid","solid"},{"semantics","solid-material-index"},
            {"transform","vdb-sample-center"},{"domain",{{"minimum",{bbox.min().x(),bbox.min().y(),bbox.min().z()}},{"maximum",{bbox.max().x(),bbox.max().y(),bbox.max().z()}}}},{"palette",palette}};
        const auto serialize_start=Clock::now(); std::ostringstream stream(std::ios::binary|std::ios::out); openvdb::io::Stream writer(stream); writer.write({solid});
        const auto bytes=stream.str(); require(bytes.size()<=64*1024*1024,"Serialized VDB exceeds import budget");
        MeshVoxelization result; result.description_json=descriptor.dump(2)+'\n';
        require(result.description_json.size()<=2*1024*1024,"Voxel descriptor exceeds import budget");
        const auto data=std::as_bytes(std::span{bytes.data(),bytes.size()}); result.vdb.assign(data.begin(),data.end());
        result.statistics_json=Json{{"algorithm","openvdb-12.0.1-unsigned-distance-shell"},{"resolution",options.resolution},{"voxel_size",voxel},
            {"triangles",triangles.size()},{"candidates",distance->activeVoxelCount()},{"occupied",solid->activeVoxelCount()},{"masked",masked},{"palette_size",palette.size()},
            {"distance_ms",distance_ms},{"bake_ms",bake_ms},{"serialize_ms",milliseconds(serialize_start)},{"total_ms",milliseconds(start)},
            {"distance_and_index_tree_bytes",temporary_bytes},{"solid_tree_bytes",solid->memUsage()},{"vdb_bytes",bytes.size()},
            {"bounds_min",{minimum.x(),minimum.y(),minimum.z()}},{"bounds_max",{maximum.x(),maximum.y(),maximum.z()}},
            {"texture_colors",options.texture_colors},{"limitations",{"Mesh-local single mesh; node placements not applied","Surface shell; no interior fill","Bilinear UV0 base color; 4-bit linear RGB","Normal/emissive/metallic-roughness textures not baked","OpenVDB scratch allocation is not hard-capped"}}}.dump(2)+'\n';
        return result;
    } catch(const Error& error) { return std::unexpected(error); }
      catch(const openvdb::Exception& error) { return std::unexpected(Error{ErrorCode::invalid_argument,error.what()}); }
}
} // namespace dk::assets
