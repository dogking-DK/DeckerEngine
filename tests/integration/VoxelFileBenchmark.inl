// File-asset performance is separate from the synthetic oracle matrix.
void run_file(Json& output,const std::filesystem::path& root,bool validation,bool smoke,const std::filesystem::path& report) {
    auto memory=take(memory::MemorySystem::create());auto heap=take(memory.create_heap({"voxel-file-benchmark",memory::DomainCategory::render}));Diagnostics diagnostics;
    {
        memory::ThreadContext context{memory};memory::ExecutionScope scope{context,heap};
        auto asset=take(assets::import_voxel(take(ProjectPaths::create(root)),{"solid.dkvoxel"}));
        output["kind"]="voxel-file-baseline";output["root"]=root.generic_string();
        output["nano_hash"]=hash_bytes(asset.voxel->grid->bytes()); output["nano_bytes"]=asset.voxel->grid->bytes().size();
        DeviceOptions options; options.validation=validation ? ValidationMode::required : ValidationMode::disabled;
        options.ray_tracing=RayTracingMode::required;options.sampled_image_indexing=SampledImageIndexingMode::required;
        options.diagnostic_sink=diagnostic;options.diagnostic_user_data=&diagnostics;
        auto q=take(SubmissionQueue::create(heap,take(Device::create(heap,options)),3));check(q.configure_gpu_profiling({true}));
        const auto& adapter=q.device().adapter();output["device"]={{"name",adapter.properties.deviceName.data()},{"driver",adapter.driver.driverInfo.data()}};
        output["device_only"]=gpu_memory(q);
        {
            auto pipeline=take(ScenePipeline::create(heap,q,DK_RENDER_SHADER_DIR));auto meshes=take(GpuAssets::create(heap));
            auto renderer=take(PathTraceRenderer::create(heap,q,DK_PATH_SHADER_DIR));JointScene scene{heap,asset};
            auto cache=take(VoxelGpuCache::create(q));auto grid=take(cache->prepare(q,*asset.voxel->grid));require(take(q.wait(grid->upload_submission())),"file upload failed");
            output["upload_gpu"]=gpu_profile(grid->upload_submission());
            std::vector<VoxelMaterial> palette;for(const auto& p:asset.voxel->palette) palette.push_back({p.base_color,p.metallic,p.roughness});
            const std::array instances{VoxelInstance{scene.entity,grid,scene.world,palette,asset.voxel->id}};
            auto start=Clock::now();auto gpu=take(renderer.prepare_scene(q,scene.path(heap,8)));require(take(q.wait(gpu.submission())),"file AS build failed");
            output["as_wall_ms"]=ms(start);output["as_gpu"]=gpu_profile(gpu.submission());output["geometry"]=geometry_memory(gpu);
            if(!smoke) {
                std::vector<Vec3d> precondition;
                output["precondition_path"]=path_view(heap,scene,renderer,q,gpu,1280,720,false,29,precondition,true);
            }
            output["views"]=Json::array();
            const std::vector<std::pair<unsigned,unsigned>> sizes=smoke ? std::vector<std::pair<unsigned,unsigned>>{{640,480}} : std::vector<std::pair<unsigned,unsigned>>{{640,480},{1280,720}};
            for(const auto [w,h]:sizes) for(bool ortho:{false,true}) {
                Json row={{"width",w},{"height",h},{"projection",ortho ? "orthographic" : "perspective"},{"raster",Json::array()}};
                std::uint64_t checksum=0;std::vector<std::byte> pixels(std::size_t(w)*h*4);
                for(unsigned i=0;i<(smoke ? 5u : 11u);++i) {
                    auto camera=view(heap,scene.scene,w,h,ortho,i+1);const auto begin=Clock::now();
                    auto frame=take(pipeline.render_voxels(q,camera,meshes,instances,{},unlit_preview_settings()));require(take(frame.wait(q)),"file raster wait failed");check(frame.read_rgba8(pixels));
                    const auto hash=hash_bytes(pixels);if(i) require(hash==checksum,"file raster is unstable");checksum=hash;
                    row["raster"].push_back({{"phase",i==0 ? "cold" : i<3 ? "warmup" : "steady"},{"wall_ms",ms(begin)},{"gpu",gpu_profile(frame.submission())}});
                }
                const auto stem=std::to_string(w)+(ortho ? "-ortho" : "-perspective");
                {std::ofstream image(report.parent_path()/(stem+"-raster.ppm"),std::ios::binary);image<<"P6\n"<<w<<' '<<h<<"\n255\n";
                    for(std::size_t i=0;i<pixels.size();i+=4) image.write(reinterpret_cast<const char*>(pixels.data()+i),3);require(bool(image),"file raster screenshot failed");}
                std::vector<Vec3d> linear;row["path"]=path_view(heap,scene,renderer,q,gpu,w,h,ortho,smoke ? 2 : 8,linear,true);
                {std::ofstream image(report.parent_path()/(stem+"-path.ppm"),std::ios::binary);image<<"P6\n"<<w<<' '<<h<<"\n255\n";
                    for(const auto& p:linear) for(int c=0;c<3;++c) {const auto value=static_cast<unsigned char>(tone(p[c]));image.write(reinterpret_cast<const char*>(&value),1);}require(bool(image),"file path screenshot failed");}
                row["raster_checksum"]=checksum;output["views"].push_back(row);save_json(report,output);
            }
        }
        check(q.poll());output["after_release"]=gpu_memory(q);
        require(output["after_release"]["allocation_bytes"]==output["device_only"]["allocation_bytes"],"file GPU resources not reclaimed");
        check(q.close());
    }
    output["diagnostics"]={{"errors",diagnostics.errors.load()},{"warnings",diagnostics.warnings.load()}};
    require(!diagnostics.errors && !diagnostics.warnings,"file GPU diagnostics");
    require(!heap.snapshot().live_allocations && memory.try_close().closed(),"file heap allocations remain");
}
