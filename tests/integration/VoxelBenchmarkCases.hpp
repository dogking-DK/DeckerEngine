#pragma once
#include "VoxelBenchmarkSupport.hpp"
namespace {
struct Box { vx::Coord lo; int dimension; };
struct Input {
    std::string name; unsigned size; vx::Domain domain;
    std::vector<Box> boxes;
    bool checker=false;
};
std::vector<Input> inputs(bool smoke) {
    std::vector<Input> result;
    for(unsigned n:{64u,128u,256u}) {
        if(smoke && n!=64) continue;
        for(std::string kind:{"empty","sparse","solid"}) {
            Input in{kind+std::to_string(n),n,{{0,0,0},{int(n)-1,int(n)-1,int(n)-1}}};
            if(kind=="solid") for(int z=0;z<int(n);z+=8) for(int y=0;y<int(n);y+=8) for(int x=0;x<int(n);x+=8) in.boxes.push_back({{x,y,z},8});
            if(kind=="sparse") for(int z=0;z<int(n);z+=32) for(int y=0;y<int(n);y+=32) for(int x=0;x<int(n);x+=32) in.boxes.push_back({{x,y,z},8});
            result.push_back(std::move(in));
        }
    }
    result.push_back({"islands",0,{{-65536,0,0},{65535,7,7}},{{{-65536,0,0},8},{{65528,0,0},8}}});
    result.push_back({"root_tiles",0,{{-4096,0,0},{4095,4095,4095}},{{{-4096,0,0},4096},{{0,0,0},4096}}});
    result.push_back({"checker64",64,{{0,0,0},{63,63,63}},{},true});
    return result;
}
openvdb::Int32Grid::Ptr source(const Input& in) {
    auto grid=openvdb::Int32Grid::create(0); auto map=openvdb::math::Transform::createLinearTransform(1);
    map->postTranslate(openvdb::Vec3d(.5)); grid->setTransform(map);
    if(in.checker) { auto access=grid->getAccessor(); for(int z=0;z<64;++z) for(int y=0;y<64;++y) for(int x=0;x<64;++x)
        if((x+y+z)&1) access.setValue(openvdb::Coord(x,y,z),1); }
    for(const auto& b:in.boxes) grid->tree().addTile(b.dimension==8 ? 1u : 3u,openvdb::Coord(b.lo[0],b.lo[1],b.lo[2]),1,false);
    return grid;
}
std::optional<vx::DenseGrid> dense(const Input& in) {
    if(!in.size) return {};
    const auto n=in.size; std::vector<std::uint32_t> cells(std::size_t(n)*n*n);
    if(in.checker) for(unsigned z=0;z<n;++z) for(unsigned y=0;y<n;++y) for(unsigned x=0;x<n;++x) cells[x+n*(y+n*z)]=(x+y+z)&1;
    else for(const auto& b:in.boxes) for(int z=b.lo[2];z<b.lo[2]+b.dimension;++z) for(int y=b.lo[1];y<b.lo[1]+b.dimension;++y) for(int x=b.lo[0];x<b.lo[0]+b.dimension;++x)
        cells[std::size_t(x)+n*(std::size_t(y)+n*std::size_t(z))]=1;
    return take(vx::DenseGrid::build({{0,0,0},{n,n,n},cells}));
}
std::optional<vx::Hit> analytic(const Input& in,const geometry::Ray& ray) {
    // The large fixtures are disjoint boxes, or two adjacent equally solid root tiles.
    std::vector<Box> boxes=in.boxes;
    if(in.name=="root_tiles") {
        // All benchmark query rays go along X, inside the tile Y/Z cross-section.
        if(ray.direction.x()==0) throw std::runtime_error("unsupported analytic ray");
        const double a=(-4096-ray.origin.x())/ray.direction.x(),b=(4096-ray.origin.x())/ray.direction.x();
        const double t=std::min(a,b)>=ray.minimum ? std::min(a,b) : std::max(a,b);
        if(t<ray.minimum || t>ray.maximum) return {};
        const bool positive=std::abs(ray.origin.x()+t*ray.direction.x()-4096)<1e-7;
        vx::Hit h; h.distance=t; h.value=1; h.face=positive ? vx::Face::positive_x : vx::Face::negative_x;
        h.cell={positive ? 4095 : -4096,int(std::floor(ray.origin.y())),int(std::floor(ray.origin.z()))}; h.normal={positive ? 1. : -1.,0,0}; return h;
    }
    std::optional<vx::Hit> best;
    for(const auto& b:boxes) {
        double enter=-1e300,leave=1e300; int enter_axis=0,leave_axis=0; bool miss=false;
        for(int a=0;a<3;++a) {
            const double lo=b.lo[a],hi=lo+b.dimension,d=ray.direction[a],o=ray.origin[a];
            if(d==0) { if(o<=lo || o>=hi) miss=true; continue; }
            double t0=(lo-o)/d,t1=(hi-o)/d; if(t0>t1) std::swap(t0,t1);
            if(t0>enter) { enter=t0; enter_axis=a; } if(t1<leave) { leave=t1; leave_axis=a; }
        }
        const bool entering=enter>=ray.minimum; const double t=entering ? enter : leave;
        if(miss || enter>=leave || t<ray.minimum || t>ray.maximum || (best && t>=best->distance)) continue;
        const int axis=entering ? enter_axis : leave_axis; const bool positive=(ray.direction[axis]>0)!=entering;
        vx::Hit h; h.distance=t; h.value=1; h.face=static_cast<vx::Face>(axis*2+int(positive)); h.normal[axis]=positive ? 1 : -1;
        const Vec3d p=ray.origin+t*ray.direction; for(int a=0;a<3;++a) h.cell[a]=int(std::floor(p[a])); h.cell[axis]=positive ? b.lo[axis]+b.dimension-1 : b.lo[axis]; best=h;
    }
    return best;
}
void compare(const VoxelGpuResult& actual,const std::optional<vx::Hit>& expected) {
    require((actual.status==VoxelGpuStatus::hit)==expected.has_value(),"oracle hit/miss mismatch");
    if(!expected) { require(actual.status==VoxelGpuStatus::miss,"GPU error treated as miss"); return; }
    require(actual.value==expected->value && actual.cell==expected->cell && actual.face==unsigned(expected->face),"oracle cell/value/face mismatch");
    require(std::abs(actual.distance-expected->distance)<=1e-4*std::max(1.,std::abs(expected->distance)),"oracle distance mismatch");
    for(int a=0;a<3;++a) require(std::abs(actual.normal[a]-expected->normal[a])<=1e-4,"oracle normal mismatch");
}
void equal(const VoxelGpuResult& a,const VoxelGpuResult& b) {
    require(a.status==b.status && a.value==b.value && a.face==b.face && a.cell==b.cell,"variant surface mismatch");
    require(a.normal==b.normal && a.distance==b.distance,"variant numeric mismatch");
}
std::vector<geometry::Ray> rays(const Input& in) {
    std::vector<geometry::Ray> result;
    for(unsigned i=0;i<1024;++i) {
        if(in.size) { const auto n=in.size;
            result.push_back({{double((i*17)%n)+.25,double((i*31+i/32)%n)+.375,-1},{0,0,1},0,double(n)+2});
        } else if(in.name=="islands") result.push_back({{-65520.25,.25+double(i%7),.375+double((i/7)%7)},{1,0,0},0,131072});
        else result.push_back({{-4090.25,.25+double(i%4095),.375+double((i*7)%4095)},{1,0,0},0,8192});
    }
    return result;
}
Json trace(SubmissionQueue& q,const VoxelGpuQuery& query,const std::shared_ptr<const VoxelGpuGrid>& grid,
    const std::vector<geometry::Ray>& input,std::vector<VoxelGpuResult>& output) {
    const auto start=Clock::now(); auto f=take(query.trace(q,grid,input)); const auto submitted=Clock::now();
    require(take(f.read(q,output)),"query incomplete");
    std::uint64_t visits=0,skips=0; for(const auto& h:output) { visits+=h.visited_regions; skips+=h.skipped_regions; }
    return {{"cpu_submit_ms",ms(start,submitted)},{"end_to_end_ms",ms(start)},{"gpu",gpu_profile(f.submission())},
        {"rays",input.size()},{"visited",visits},{"skipped",skips}};
}
VoxelGpuQuery variant(memory::ResourceHandle heap,SubmissionQueue& q,const std::filesystem::path& root,const char* name,const char* define) {
    const auto dir=root/name; std::filesystem::create_directories(dir);
    std::ofstream f(dir/"voxel-query.slang"); f<<"#define "<<define<<" 1\n#include \""<<(std::filesystem::path{DK_VOXEL_SHADER_DIR}/"voxel-query.slang").generic_string()<<"\"\n"; f.close();
    return take(VoxelGpuQuery::create(heap,q,dir));
}
Json queries(const Input& in,const std::optional<vx::DenseGrid>& oracle,SubmissionQueue& q,
    const std::shared_ptr<const VoxelGpuGrid>& grid,const VoxelGpuQuery& tuned,const VoxelGpuQuery& baseline,const VoxelGpuQuery& scalar,unsigned samples,unsigned round) {
    auto input=rays(in); std::vector<VoxelGpuResult> a,b;
    auto first=trace(q,tuned,grid,input,a);
    for(std::size_t i=0;i<input.size();++i) compare(a[i],oracle ? take(oracle->nearest(input[i])).hit : analytic(in,input[i]));
    Json result={{"oracle_rays",input.size()},{"oracle",oracle ? "dense_dda" : "analytic_boxes"},{"first",first},{"samples",Json::array()}};
    for(unsigned i=0;i<samples+2;++i) {
        Json x,y;
        if((i+round)%2) { x=trace(q,tuned,grid,input,a); y=trace(q,baseline,grid,input,b); }
        else { y=trace(q,baseline,grid,input,b); x=trace(q,tuned,grid,input,a); }
        for(std::size_t j=0;j<a.size();++j) equal(a[j],b[j]);
        result["samples"].push_back({{"phase",i<2 ? "warmup" : "steady"},{"tuned",x},{"baseline",y}});
    }
    input.resize(32); if(!in.size) for(auto& r:input) r.maximum=4096;
    auto hdda=trace(q,tuned,grid,input,a),leaf=trace(q,scalar,grid,input,b);
    for(std::size_t i=0;i<a.size();++i) equal(a[i],b[i]);
    result["ablation"]={{"maximum_t",input[0].maximum},{"hdda",hdda},{"cell_step",leaf}};
    // Semantics-only checksum excludes backend-dependent traversal counters/padding.
    Json hits=Json::array(); for(const auto& h:a) hits.push_back({unsigned(h.status),h.value,h.face,h.cell,h.distance,h.normal});
    const auto text=hits.dump(); result["checksum"]=hash_bytes(std::as_bytes(std::span{text.data(),text.size()})); return result;
}
RenderView view(memory::ResourceHandle heap,const RenderScene& scene,unsigned width,unsigned height,bool ortho,std::uint64_t frame) {
    ViewDescription d{width,height,frame}; d.projection=ortho ? take(orthographic_projection({3.,.1,20},double(width)/height)) :
        take(perspective_projection({std::numbers::pi/3,.1,20},double(width)/height));
    (void)heap; return take(RenderView::create(scene,d));
}
Json image_quality(const RenderFrame& f,const RenderView& v,const vx::DenseGrid& oracle,const Transformd& world,const RenderSettings& settings) {
    const auto info=f.info(); std::vector<std::byte> pixels(std::size_t(info.width)*info.height*4); check(f.read_rgba8(pixels));
    std::vector<float> depths(settings.depth_readback ? pixels.size()/4 : 0); if(settings.depth_readback) check(f.read_depth32f(depths));
    const Mat4d unproject=v.world_to_clip().inverse(); const auto inverse=take(world.inverse()); unsigned hits=0,parallel_plane_rays=0; int color_error=0; double depth_error=0; std::string mismatch;
    for(unsigned i=0;i<1024;++i) {
        const unsigned x=(i*619+17)%info.width,y=(i*397+23)%info.height;
        const double sx=2*(x+.5)/info.width-1,sy=2*(y+.5)/info.height-1;
        Vec4d a=unproject*Vec4d{sx,sy,0,1},b=unproject*Vec4d{sx,sy,1,1}; a/=a.w(); b/=b.w(); const Vec3d delta=(b-a).head<3>();
        geometry::Ray ray{a.head<3>(),delta.normalized(),0,delta.norm()};
        geometry::Ray local{take(inverse.transform_point(ray.origin)),take(inverse.transform_direction(ray.direction)),ray.minimum,ray.maximum};
        bool canonicalized=false;
        // Exact parallel grid planes belong to the positive-side cell. Undo only
        // double matrix inversion roundoff, not float ray error or an edge mask.
        for(int axis=0;axis<3;++axis) if(local.direction[axis]==0) {
            const double plane=std::round(local.origin[axis]);
            if(std::abs(local.origin[axis]-plane)<=64*std::numeric_limits<double>::epsilon()*std::max(1.,std::abs(plane))) {
                local.origin[axis]=plane; canonicalized=true;
            }
        }
        if(canonicalized) ++parallel_plane_rays;
        const auto hit=take(oracle.nearest(local)).hit;
        double depth=1; Vec3d color=settings.clear_rgb.cast<double>(); if(hit) {
            ++hits; color={.125,.5,1}; const Vec3d p=ray.origin+ray.direction*hit->distance; const Vec4d clip=v.world_to_clip()*Vec4d{p.x(),p.y(),p.z(),1}; depth=clip.z()/clip.w();
        }
        const std::size_t pixel=std::size_t(y)*info.width+x;
        for(unsigned c=0;c<3;++c) color_error=std::max(color_error,std::abs(std::to_integer<int>(pixels[pixel*4+c])-tone(color[c])));
        if(settings.depth_readback) depth_error=std::max(depth_error,std::abs(depths[pixel]-depth));
        if(mismatch.empty() && (color_error>1 || depth_error>2e-6)) mismatch=" raster="+std::to_string(info.width)+"x"+std::to_string(info.height)+
            " pixel="+std::to_string(x)+","+std::to_string(y)+" color_error="+std::to_string(color_error)+" depth_error="+std::to_string(depth_error)+
            " expected="+(hit ? std::to_string(hit->distance) : "miss")+" world_origin="+std::to_string(ray.origin.x())+","+std::to_string(ray.origin.y())+","+std::to_string(ray.origin.z());
    }
    if(color_error>1 || depth_error>2e-6) throw std::runtime_error("raster dense oracle mismatch"+mismatch);
    return {{"pixels",1024},{"hits",hits},{"parallel_plane_rays",parallel_plane_rays},{"maximum_color_error_bytes",color_error},{"maximum_depth_error",depth_error}};
}
Json render_case(const Input& in,const vx::SparseGrid& cpu,const std::optional<vx::DenseGrid>& oracle,memory::ResourceHandle heap,SubmissionQueue& q,
    ScenePipeline& pipeline,const GpuAssets& meshes,const RenderScene& scene,const std::shared_ptr<const VoxelGpuGrid>& grid,unsigned w,unsigned h,bool ortho,unsigned samples,EntityId entity={},AssetId asset={}) {
    Vec3d extent,translation,scale; for(int a=0;a<3;++a) { extent[a]=double(in.domain.maximum[a])-in.domain.minimum[a]+1; scale[a]=2/extent[a]; translation[a]=-1-scale[a]*in.domain.minimum[a]; }
    translation.z()-=5; const auto world=transform(translation,scale);
    const std::array<VoxelMaterial,1> palette{{{{.125f,.5f,1},0,1}}};
    const std::array instances{VoxelInstance{entity.is_nil() ? take(EntityId::parse("00000000-0000-4000-8000-000000000001")) : entity,grid,world,palette,asset}};
    Json result={{"width",w},{"height",h},{"projection",ortho ? "orthographic" : "perspective"},{"frames",Json::array()}};
    std::uint64_t checksum=0;
    for(unsigned i=0;i<samples+3;++i) {
        auto v=view(heap,scene,w,h,ortho,i+1); const auto settings=unlit_preview_settings();
        const auto start=Clock::now(); auto frame=take(pipeline.render_voxels(q,v,meshes,instances,{},settings)); const auto submitted=Clock::now();
        require(take(frame.wait(q)),"raster wait failed"); const auto waited=Clock::now();
        std::vector<std::byte> pixels(std::size_t(w)*h*4); check(frame.read_rgba8(pixels)); const auto done=Clock::now();
        const auto hash=hash_bytes(pixels); if(i) require(hash==checksum,"unstable repeated raster frame"); checksum=hash;
        result["frames"].push_back({{"phase",i==0 ? "cold" : i<3 ? "warmup" : "steady"},{"cpu_submit_ms",ms(start,submitted)},
            {"wait_ms",ms(submitted,waited)},{"readback_ms",ms(waited,done)},{"end_to_end_ms",ms(start,done)},{"gpu",gpu_profile(frame.submission())}});
    }
    RenderSettings diagnostic_settings=unlit_preview_settings();
    auto v=view(heap,scene,w,h,ortho,100); auto prior=take(pipeline.render_voxels(q,v,meshes,instances,{},diagnostic_settings)); require(take(prior.wait(q)),"quality wait failed");
    result["quality"]=oracle ? image_quality(prior,v,*oracle,world,diagnostic_settings) : Json{{"pixels",0},{"oracle","large-domain analytic query only"}};
    if(oracle) {
        // Respect the production diagnostic-readback cap (65536 pixels). Full-size
        // color is checked above; depth uses the same projection/aspect at 256 wide.
        diagnostic_settings.depth_readback=true; auto depth_view=view(heap,scene,256,256*h/w,ortho,101);
        auto depth_frame=take(pipeline.render_voxels(q,depth_view,meshes,instances,{},diagnostic_settings)); require(take(depth_frame.wait(q)),"depth quality wait failed");
        const auto quality=image_quality(depth_frame,depth_view,*oracle,world,diagnostic_settings);
        result["quality"]["depth_pixels"]=quality["pixels"]; result["quality"]["depth_extent"]={256,256*h/w};
        result["quality"]["depth_parallel_plane_rays"]=quality["parallel_plane_rays"];
        result["quality"]["maximum_depth_error"]=quality["maximum_depth_error"];
    }
    auto pending=take(pipeline.render_voxels(q,v,meshes,instances)); result["old_and_pending_frame"]=gpu_memory(q);
    require(take(pending.wait(q)),"pending frame wait failed"); prior={}; pending={}; check(q.poll()); result["after_frames"]=gpu_memory(q);
    result["checksum"]=checksum; result["nano_gpu_payload_bytes"]=cpu.bytes().size(); result["palette_payload_bytes"]=sizeof(VoxelMaterial);
    return result;
}
}
