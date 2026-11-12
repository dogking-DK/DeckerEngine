#include "VoxelBenchmarkCases.hpp"
#include "../PathVoxelSupport.hpp"
#include <dk/render/PathTraceRenderer.hpp>
#include <dk/graphics/Transfer.hpp>
namespace {
std::uint64_t shader_identity() {
    std::uint64_t hash=14695981039346656037ull;
    for(const auto* directory:{DK_RENDER_SHADER_DIR,DK_VOXEL_SHADER_DIR,DK_PATH_SHADER_DIR,DK_RTXDI_INCLUDE_DIR}) {
        std::vector<std::filesystem::path> files;
        for(const auto& entry:std::filesystem::recursive_directory_iterator(directory)) if(entry.is_regular_file()) files.push_back(entry.path());
        std::sort(files.begin(),files.end());
        for(const auto& file:files) {
            for(unsigned char c:file.lexically_relative(directory).generic_string()) hash=(hash^c)*1099511628211ull;
            std::ifstream stream(file,std::ios::binary);require(bool(stream),"shader file unavailable");char c;
            while(stream.get(c)) hash=(hash^static_cast<unsigned char>(c))*1099511628211ull;
        }
    }
    std::ifstream nano(std::filesystem::path{DK_JOINT_NANO_INCLUDE_DIR}/"nanovdb/PNanoVDB.h",std::ios::binary);require(bool(nano),"PNanoVDB header missing");char c;
    while(nano.get(c)) hash=(hash^static_cast<unsigned char>(c))*1099511628211ull;
    return hash;
}
Json owned_memory(const PathTraceRenderer& r,const SubmissionQueue& q) {
    const auto m=r.memory_stats(q); auto j=gpu_memory(q);
    j["owned"]={{"current",m.owned.current},{"peak",m.owned.peak},{"limit",m.owned.limit}};
    j["sbt_region_bytes"]=m.sbt_region_bytes; return j;
}
Json geometry_memory(const PathTraceGpuScene& scene) {
    const auto m=scene.memory_info(); return {{"triangles",m.triangles},{"nano",m.nano},{"instances",m.instances},{"blocks",m.blocks},{"palette",m.palette},
        {"aabb_input",m.aabb_input},{"blas_storage",m.blas_storage},{"tlas_storage",m.tlas_storage},{"as_scratch_requirement",m.as_scratch}};
}
PathTraceGpuImage read_path(SubmissionQueue& q,const PathTraceFrame& frame) {
    require(take(q.wait(frame.submission())),"path readback wait failed"); auto image=take(frame.read(q)); require(bool(image),"path readback missing"); return std::move(*image);
}
Json advance(PathTraceRenderer& r,SubmissionQueue& q) {
    Json tiles=Json::array(); double gpu=0,record=0,wait=0; const auto start=Clock::now();
    while(take(r.status(1)).completed_spp<take(r.status(1)).target_spp) {
        const auto before=Clock::now(); require(take(r.submit_batch(q,1)),"no path tile"); const auto submitted=Clock::now();
        auto ticket=r.pending_submission(); require(take(q.wait(ticket)),"path tile wait failed"); (void)take(r.poll(q)); const auto done=Clock::now();
        auto profile=gpu_profile(ticket); gpu+=profile["submission_ms"].get<double>(); record+=ms(before,submitted); wait+=ms(submitted,done);
        tiles.push_back({{"record_submit_ms",ms(before,submitted)},{"wait_poll_ms",ms(submitted,done)},{"gpu",profile}});
    }
    return {{"gpu_ms",gpu},{"cpu_submit_ms",record},{"wait_ms",wait},{"end_to_end_ms",ms(start)},{"tiles",tiles}};
}
Transformd world_for(const Input& in) {
    Vec3d scale,offset; for(int a=0;a<3;++a) {scale[a]=2./(double(in.domain.maximum[a])-in.domain.minimum[a]+1);offset[a]=-1-scale[a]*in.domain.minimum[a];}
    offset.z()-=5; return transform(offset,scale);
}
struct JointScene {
    CpuAsset asset;
    std::unique_ptr<SceneDocument> document;
    RenderScene scene;
    Transformd world;
    EntityId entity;
    JointScene(memory::ResourceHandle heap,const Input& in,const vx::SparseGrid& grid) {
        document=take(SceneDocument::create(take(SceneId::parse("01000000-0000-4000-8000-000000000013"))));
        asset.voxel.emplace();asset.voxel->id=take(AssetId::parse("02000000-0000-4000-8000-000000000013"));
        asset.voxel->grid=std::make_shared<const vx::SparseGrid>(grid);asset.voxel->palette={{Vec3f{.125f,.5f,1.f},0,1}};
        entity=take(EntityId::parse("03000000-0000-4000-8000-000000000013"));check(document->create_entity(entity));
        check(document->set_asset_references(entity,{{asset.voxel->id,AssetKind::voxel}}));world=world_for(in);
        Trsd trs;trs.translation=world.matrix().block<3,1>(0,3);trs.scale=world.matrix().block<3,3>(0,0).diagonal();check(document->set_local_transform(entity,trs));
        scene=take(RenderScene::extract(heap,take(document->snapshot())));
    }
    PathTraceScene path(memory::ResourceHandle heap,unsigned edge) const {
        auto limits=desktop_path_scene_limits();limits.voxel_block_edge=edge;const std::array<const CpuAsset*,1> assets{&asset};
        return take(PathTraceScene::create(heap,scene,assets,limits));
    }
};
std::vector<geometry::Ray> joint_rays(const Input& in) {
    if(in.name!="islands16k") return rays(in);
    std::vector<geometry::Ray> result;for(unsigned i=0;i<1024;++i) result.push_back({{-8176.25,.25+double(i%7),.375+double((i/7)%7)},{1,0,0},0,16384});return result;
}
Json primary_quality(const Input& in,const std::optional<vx::DenseGrid>& oracle,const JointScene& scene,PathTraceRenderer& renderer,SubmissionQueue& q,
    const PathTraceGpuScene& gpu,const VoxelGpuQuery& query,const std::shared_ptr<const VoxelGpuGrid>& grid) {
    const auto local=joint_rays(in);std::vector<geometry::Ray> world;std::vector<PathTraceRay> path;std::vector<std::optional<vx::Hit>> expected;
    for(const auto& ray:local) {
        auto h=oracle ? take(oracle->nearest(ray)).hit : analytic(in,ray);
        const auto direction=take(scene.world.transform_direction(ray.direction));const auto length=direction.norm();
        const auto origin=take(scene.world.transform_point(ray.origin));
        world.push_back({origin,direction/length,ray.minimum*length,ray.maximum*length});path.push_back({origin,direction/length,ray.minimum*length,ray.maximum*length});
        if(h) h->distance*=length;expected.push_back(h);
    }
    std::vector<VoxelGpuResult> compute;auto frame=take(query.trace(q,grid,world,scene.world));require(take(frame.read(q,compute)),"compute primary incomplete");
    PathTraceSettings settings;settings.max_bounces=1;settings.rr_start=1;settings.environment_rgb={0,0,0};settings.sun=PathTraceSun::none;
    auto image=read_path(q,take(renderer.submit_rays(q,gpu,path,settings,{0,1})));std::uint64_t hits=0,visited=0,skipped=0;double error=0;
    for(std::size_t i=0;i<world.size();++i) {
        compare(compute[i],expected[i]);visited+=compute[i].visited_regions;skipped+=compute[i].skipped_regions;
        const auto& actual=image.primary_hits()[i];require(bool(actual)==bool(expected[i]),"RT oracle presence mismatch");
        if(!actual) continue;++hits;const auto& h=*expected[i];
        require(actual->kind==PathGeometryKind::voxel && actual->cell==h.cell && actual->value==h.value && actual->face==unsigned(h.face),"RT oracle identity mismatch");
        error=std::max(error,std::abs(actual->distance-h.distance));
        require(std::abs(actual->distance-h.distance)<=1e-4*std::max(1.,std::abs(h.distance)),"RT oracle distance mismatch");
        const Vec3d facing=path[i].direction.dot(h.normal)<0 ? h.normal : -h.normal;
        require((actual->normal-facing).cwiseAbs().maxCoeff()<=1e-4,"RT oracle facing normal mismatch");
    }
    return {{"rays",world.size()},{"hits",hits},{"max_distance_error",error},{"compute_visited",visited},{"compute_skipped",skipped},{"oracle",oracle ? "dense_dda" : "analytic_boxes"}};
}
Json path_view(memory::ResourceHandle heap,const JointScene& scene,PathTraceRenderer& renderer,SubmissionQueue& q,const PathTraceGpuScene& gpu,
    unsigned w,unsigned h,bool ortho,unsigned samples,std::vector<Vec3d>& reference,bool native) {
    const auto camera=view(heap,scene.scene,w,h,ortho,1);PathTraceSettings settings;settings.seed=2313;settings.max_bounces=4;settings.rr_start=3;
    settings.environment_rgb={.7,.8,.9};settings.sun=PathTraceSun::none;
    Json result={{"width",w},{"height",h},{"projection",ortho ? "orthographic" : "perspective"},{"frames",Json::array()}};
    for(unsigned i=0;i<samples+3;++i) {
        PathTraceSchedule schedule;schedule.target_spp=i+1;schedule.tile_edge=256;schedule.tiles_per_submission=4;
        check(renderer.configure_view(q,1,gpu,camera,settings,schedule));auto frame=advance(renderer,q);
        frame["phase"]=i==0 ? "cold" : i<3 ? "warmup" : "steady";frame["spp"]=i+1;result["frames"].push_back(std::move(frame));
    }
    {
        const auto start=Clock::now();auto display=take(renderer.display(q,renderer.completed_frame(1),0,true));require(take(q.wait(display.submission)),"tone wait failed");
        result["tone"]={{"wall_ms",ms(start)},{"gpu",gpu_profile(display.submission)}};
    }
    {
        const auto start=Clock::now();auto read=take(renderer.completed_frame(1).read_linear(q));auto image=read_path(q,read);double error=0,scaled_error=0;
        require(image.info().samples.count==samples+3,"wrong final spp");
        for(std::size_t i=0;i<image.linear_rgb().size();++i) {
            const auto& v=image.linear_rgb()[i];require(v.allFinite() && v.minCoeff()>=0,"invalid path output");
            if(!native) {require(reference.size()==image.linear_rgb().size(),"missing native image");const auto d=(v-reference[i]).cwiseAbs();error=std::max(error,d.maxCoeff());
                scaled_error=std::max(scaled_error,(d.array()/reference[i].cwiseAbs().cwiseMax(1.).array()).maxCoeff());
                if(!(d.array()<=2e-4*reference[i].cwiseAbs().cwiseMax(1.).array()).all())
                    std::fprintf(stderr,"layout pixel domain_max_x=%d size=%ux%u ortho=%u pixel=%zu got=%g,%g,%g expected=%g,%g,%g\n",scene.asset.voxel->grid->domain().maximum[0],w,h,unsigned(ortho),i,v.x(),v.y(),v.z(),reference[i].x(),reference[i].y(),reference[i].z());
                require((d.array()<=2e-4*reference[i].cwiseAbs().cwiseMax(1.).array()).all(),"pooled layout changed radiance");}
        }
        if(native) reference.assign(image.linear_rgb().begin(),image.linear_rgb().end());
        result["linear"]={{"wall_ms",ms(start)},{"gpu",gpu_profile(read.submission())},{"pixels",image.linear_rgb().size()},{"max_layout_error",error},{"max_scaled_layout_error",scaled_error}};
    }
    result["memory_steady"]=owned_memory(renderer,q);
    {
        auto old=renderer.completed_frame(1);PathTraceSchedule schedule;schedule.target_spp=samples+4;schedule.tile_edge=256;schedule.tiles_per_submission=4;
        check(renderer.configure_view(q,1,gpu,camera,settings,schedule));require(take(renderer.submit_batch(q,1)),"missing in-flight candidate");
        result["memory_old_and_pending"]=owned_memory(renderer,q);const auto ticket=renderer.pending_submission();check(renderer.cancel(1));
        require(take(q.wait(ticket)),"pending cancel wait failed");require(!take(renderer.poll(q)),"cancelled frame published");
        require(old.info().samples.count==samples+3 && take(renderer.status(1)).completed_spp==samples+3,"old frame changed");
    }
    check(renderer.release_view(1));check(q.poll());result["memory_released_view"]=owned_memory(renderer,q);return result;
}
Json statistical_quality(memory::ResourceHandle heap,PathTraceRenderer& r,SubmissionQueue& q) {
    path_voxel_test::Fixture f{heap};const std::array cube{assets::VoxelBlock{{0,0,0},1,1}};f.grid(cube);f.quad(-.5f,1.5f,1.5f,{.7f,.6f,.5f});
    Json rows=Json::array();const std::array<PathTraceRay,2> rays{{{{.37,.63,1.25},{0,0,-1}},{{.37,.63,1.25},{0,0,1}}}};
    for(const auto model:{PathTraceMaterialModel::lambert,PathTraceMaterialModel::metallic_roughness}) {
        if(model==PathTraceMaterialModel::metallic_roughness) {f.voxel.voxel->palette[0].metallic=.6f;f.voxel.voxel->palette[0].roughness=.35f;}
        auto oracle=f.oracle(model);auto triangle=take(r.prepare_scene(q,oracle));
        for(unsigned edge:{8u,32u,128u}) {
            auto limits=PathTraceSceneLimits{};limits.voxel_block_edge=edge;auto cpu=f.scene(model,limits);auto native=take(r.prepare_scene(q,cpu));
            Json row={{"model",model==PathTraceMaterialModel::lambert ? "lambert" : "ggx"},{"edge",edge},{"seeds",Json::array()}};
            for(unsigned i=0;i<8;++i) {
                PathTraceSettings settings;settings.seed=101+7919*i;settings.max_bounces=4;settings.rr_start=0;settings.environment_rgb={.7,.8,.9};settings.sun=PathTraceSun::none;
                auto image=read_path(q,take(r.submit_rays(q,native,rays,settings,{0,4096})));
                auto tri=read_path(q,take(r.submit_rays(q,triangle,rays,settings,{0,4096})));Json sample={{"seed",settings.seed},{"spp",4096},{"rays",Json::array()}};
                for(std::size_t j=0;j<rays.size();++j) {
                    const auto expected=take(estimate_path_reference(oracle,rays[j],settings,{0,4096},std::uint32_t(j)));
                    if((image.linear_rgb()[j]-expected.mean).cwiseAbs().maxCoeff()>.006)
                        std::fprintf(stderr,"mean model=%u edge=%u seed=%u ray=%zu GPU=%g,%g,%g CPU=%g,%g,%g\n",unsigned(model),edge,i,j,image.linear_rgb()[j].x(),image.linear_rgb()[j].y(),image.linear_rgb()[j].z(),expected.mean.x(),expected.mean.y(),expected.mean.z());
                    require((image.linear_rgb()[j]-expected.mean).cwiseAbs().maxCoeff()<=.006,"native statistical mean mismatch");
                    require((tri.linear_rgb()[j]-expected.mean).cwiseAbs().maxCoeff()<=.006,"triangle statistical mean mismatch");
                    require((image.second_moment()[j]-expected.second_moment).cwiseAbs().maxCoeff()<=.008,"native statistical second moment mismatch");
                    auto values=[](const Vec3d& v){return Json{v.x(),v.y(),v.z()};};
                    sample["rays"].push_back({{"cpu",values(expected.mean)},{"cpu_second",values(expected.second_moment)},{"native",values(image.linear_rgb()[j])},{"triangle",values(tri.linear_rgb()[j])},{"native_second",values(image.second_moment()[j])}});
                }
                row["seeds"].push_back(sample);
            }
            rows.push_back(row);
        }
    }
    return rows;
}
void run(Json& output,bool smoke,bool validation,unsigned round,const std::filesystem::path& path) {
    openvdb::initialize();auto memory=take(memory::MemorySystem::create());auto heap=take(memory.create_heap({"voxel-joint",memory::DomainCategory::render}));Diagnostics diagnostics;
    {
        memory::ThreadContext context{memory};memory::ExecutionScope scope{context,heap};DeviceOptions options;
        options.validation=validation ? ValidationMode::required : ValidationMode::disabled;options.ray_tracing=RayTracingMode::required;options.sampled_image_indexing=SampledImageIndexingMode::required;
        options.diagnostic_sink=diagnostic;options.diagnostic_user_data=&diagnostics;const auto start=Clock::now();
        auto q=take(SubmissionQueue::create(heap,take(Device::create(heap,options)),3));check(q.configure_gpu_profiling({true}));output["device_queue_ms"]=ms(start);
        const auto& a=q.device().adapter();output["device"]={{"name",a.properties.deviceName.data()},{"driver",a.driver.driverInfo.data()},{"vendor_id",a.properties.vendorID},{"device_id",a.properties.deviceID}};
        output["device_only"]=gpu_memory(q);
        {
            const auto program_start=Clock::now();auto query=take(VoxelGpuQuery::create(heap,q));auto pipeline=take(ScenePipeline::create(heap,q,DK_RENDER_SHADER_DIR));auto meshes=take(GpuAssets::create(heap));
            auto renderer=take(PathTraceRenderer::create(heap,q,DK_PATH_SHADER_DIR));output["program_create_ms"]=ms(program_start);
            output["statistics"]=smoke ? statistical_quality(heap,renderer,q) : Json(nullptr);
            auto matrix=inputs(false);
            if(smoke) std::erase_if(matrix,[](const Input& in){return in.size>64 && !in.name.starts_with("sparse") && in.name!="solid256";});
            for(auto& in:matrix) if(in.name=="islands") in={"islands16k",0,{{-8192,0,0},{8191,7,7}},{{{-8192,0,0},8},{{8184,0,0},8}}};
            {
                Input in{"precondition",64,{{0,0,0},{63,63,63}},{{{0,0,0},8}}};assets::VoxelBuildOptions build;build.domain=in.domain;auto raw=source(in);auto cpu=take(assets::detail::convert_openvdb(*raw,build));
                JointScene scene{heap,in,cpu};auto cache=take(VoxelGpuCache::create(q));auto grid=take(cache->prepare(q,cpu));require(take(q.wait(grid->upload_submission())),"warm upload failed");
                output["precondition_raster"]=render_case(in,cpu,dense(in),heap,q,pipeline,meshes,scene.scene,grid,smoke ? 64 : 1280,smoke ? 48 : 720,false,smoke ? 0 : 61,scene.entity,scene.asset.voxel->id);
                auto gpu=take(renderer.prepare_scene(q,scene.path(heap,8)));std::vector<Vec3d> pixels;
                output["precondition_path"]=path_view(heap,scene,renderer,q,gpu,smoke ? 64 : 640,smoke ? 48 : 480,false,smoke ? 0 : 29,pixels,true);
            }
            check(q.poll());output["cases"]=Json::array();
            for(const auto& in:matrix) {
                std::printf("joint round %u %s\n",round,in.name.c_str());std::fflush(stdout);Json row={{"name",in.name},{"domain_min",in.domain.minimum},{"domain_max",in.domain.maximum}};
                const auto before=gpu_memory(q);
                {
                    MemorySampler sampler;auto begin=Clock::now();auto raw=source(in);row["source_build_ms"]=ms(begin);assets::VoxelBuildOptions build;build.domain=in.domain;
                    begin=Clock::now();auto cpu=take(assets::detail::convert_openvdb(*raw,build));row["conversion_ms"]=ms(begin);row["conversion_process_memory"]=sampler.finish();
                    const auto& st=cpu.statistics();const auto* nano=reinterpret_cast<const nanovdb::NanoGrid<std::int32_t>*>(cpu.bytes().data());
                    row["storage"]={{"source_tree_bytes",st.source_tree_bytes},{"normalized_tree_bytes",st.normalized_tree_bytes},{"nano_bytes",st.nano_bytes},{"nodes",st.node_counts},
                        {"tiles",{nano->tree().activeTileCount(1),nano->tree().activeTileCount(2),nano->tree().activeTileCount(3)}},{"active_voxels",st.active_voxels},{"content_hash",hash_bytes(cpu.bytes())}};
                    const auto oracle=dense(in);JointScene scene{heap,in,cpu};auto cache=take(VoxelGpuCache::create(q));begin=Clock::now();auto grid=take(cache->prepare(q,cpu));const auto submitted=Clock::now();
                    require(take(q.wait(grid->upload_submission())),"raster upload failed");row["raster_upload"]={{"cpu_submit_ms",ms(begin,submitted)},{"wall_ms",ms(begin)},{"gpu",gpu_profile(grid->upload_submission())}};
                    row["raster"]=Json::array();const unsigned samples=smoke ? 2 : 8;const std::vector<std::pair<unsigned,unsigned>> sizes=smoke ? std::vector<std::pair<unsigned,unsigned>>{{640,480}} : std::vector<std::pair<unsigned,unsigned>>{{640,480},{1280,720}};
                    for(const auto [w,h]:sizes) for(bool ortho:{false,true}) row["raster"].push_back(render_case(in,cpu,oracle,heap,q,pipeline,meshes,scene.scene,grid,w,h,ortho,samples,scene.entity,scene.asset.voxel->id));
                    row["layouts"]=Json::array();std::vector<std::vector<Vec3d>> references(2*sizes.size()),regression_references(2);PathTraceGpuScene previous;
                    for(unsigned edge:{8u,32u,128u}) {
                        std::printf("layout %u\n",edge);std::fflush(stdout);
                        Json layout={{"edge",edge},{"views",Json::array()}};begin=Clock::now();auto source=scene.path(heap,edge);layout["cpu_scene_ms"]=ms(begin);
                        layout["block_count"]=take(cpu.surface_blocks(1024*1024,edge)).size();begin=Clock::now();auto gpu=take(renderer.prepare_scene(q,source));const auto prepared=Clock::now();
                        layout["memory_build_with_old"]=owned_memory(renderer,q);layout["had_old_geometry"]=bool(previous);
                        const bool reused=previous && previous.submission().value()==gpu.submission().value();layout["geometry_reused"]=reused;
                        if(gpu.submission().value()) require(take(q.wait(gpu.submission())),"AS build wait failed");
                        layout["prepare"]={{"cpu_submit_ms",ms(begin,prepared)},{"wall_ms",ms(begin)},{"gpu",gpu.submission().value() && !reused ? gpu_profile(gpu.submission()) : Json(nullptr)}};
                        previous={};check(q.poll());layout["geometry"]=geometry_memory(gpu);layout["memory_scene"]=owned_memory(renderer,q);
                        layout["primary"]=primary_quality(in,oracle,scene,renderer,q,gpu,query,grid);
                        std::size_t at=0;for(const auto [w,h]:sizes) for(bool ortho:{false,true}) layout["views"].push_back(path_view(heap,scene,renderer,q,gpu,w,h,ortho,samples,references[at++],edge==8));
                        // The longer 720p stream caught block-entry rounding missed by the
                        // short 480p smoke. Keep it in the required validation gate.
                        if(smoke && (in.name.starts_with("sparse") || in.name=="solid256")) {
                            layout["regression"]=Json::array();std::size_t index=0;
                            for(const auto [w,h]:{std::pair{640u,480u},std::pair{1280u,720u}})
                                layout["regression"].push_back(path_view(heap,scene,renderer,q,gpu,w,h,false,8,regression_references[index++],edge==8));
                        }
                        previous=std::move(gpu);row["layouts"].push_back(layout);
                    }
                }
                check(q.poll());row["before"]=before;row["after_release"]=gpu_memory(q);
                require(row["after_release"]["allocation_bytes"]==before["allocation_bytes"] && row["after_release"]["allocations"]==before["allocations"],"case resources not reclaimed");
                output["cases"].push_back(row);save_json(path,output);
            }
            // Original M23.7 131072-cell span is beyond the declared native float-offset domain.
            const Input wide{"islands",0,{{-65536,0,0},{65535,7,7}},{{{-65536,0,0},8},{{65528,0,0},8}}};auto raw=source(wide);assets::VoxelBuildOptions build;build.domain=wide.domain;
            auto cpu=take(assets::detail::convert_openvdb(*raw,build));JointScene scene{heap,wide,cpu};const auto before=gpu_memory(q);auto rejected=renderer.prepare_scene(q,scene.path(heap,8));
            require(!rejected,"unsupported precision span silently accepted");check(q.poll());
            require(gpu_memory(q)["allocation_bytes"]==before["allocation_bytes"],"rejected candidate leaked");
            output["precision_rejection"]={{"span",131072},{"reason",rejected.error().message},{"code",unsigned(rejected.error().code)}};
        }
        check(q.poll());output["after_release"]=gpu_memory(q);require(output["after_release"]["allocations"]==output["device_only"]["allocations"],"joint GPU resource leak");check(q.close());
    }
    output["heap_live_allocations"]=heap.snapshot().live_allocations;output["diagnostics"]={{"errors",diagnostics.errors.load()},{"warnings",diagnostics.warnings.load()}};
    require(!diagnostics.errors && !diagnostics.warnings && !heap.snapshot().live_allocations && memory.try_close().closed(),"joint validation/CPU lifetime failure");
}
}
int main(int argc,char** argv) {
    Json output={{"schema",1},{"kind","voxel-joint-baseline"},{"matrix_version",2},{"measurement_version",1},{"status","failed"}};std::filesystem::path path;
    try {
        bool smoke=false,validation=false;unsigned round=0;
        for(int i=1;i<argc;++i) {const std::string_view arg=argv[i];if(arg=="--smoke") smoke=true;else if(arg=="--validation") validation=true;
            else if(arg=="--output" && i+1<argc) path=argv[++i];else if(arg=="--round" && i+1<argc) {const std::string s=argv[++i];require(s.size()==1 && s[0]>='0' && s[0]<='2',"round must be 0..2");round=unsigned(s[0]-'0');}else throw std::runtime_error("unknown argument");}
        require(!path.empty(),"--output required");std::filesystem::create_directories(path.parent_path());
        output.update({{"smoke",smoke},{"validation",validation},{"round",round},{"configuration",DK_BENCH_CONFIG},{"compiler",DK_BENCH_COMPILER},
            {"steady_samples",smoke ? 2 : 8},{"warmup",2},{"max_bounces",4},{"seed",2313},{"tracy_connected",profiling::is_connected()}});
        output["shader_fingerprint"]=shader_identity();run(output,smoke,validation,round,path);require(output["shader_fingerprint"]==shader_identity(),"shader closure changed during run");output["status"]="passed";
    } catch(const std::exception& error) {output["error"]=error.what();std::fprintf(stderr,"joint benchmark: %s\n",error.what());}
    if(!path.empty()) {try {save_json(path,output);}catch(...) {return 1;}}return output["status"]=="passed" ? 0 : 1;
}
