// Independent Classic MIS reference at the exact cameras of native motion images.
int direct_reference_probe(const std::filesystem::path& root,const std::filesystem::path& output,bool validation) {
    Json report={{"status","failed"},{"validation",validation},{"images",Json::array()}};
    try {
        require(std::filesystem::is_regular_file(root/".dk-editor-smoke"),"Reference requires a disposable project");
        std::ifstream input(root/".dk-direct-reference.json"); Json config; input>>config;
        auto model=take(Workspace::create(root)); check(model->open("project.json"));
        check(model->preset_environment("clear")); check(model->apply_environment());
        auto system=memory::MemorySystem::create().value(); auto heap=system.create_heap({"direct-reference",memory::DomainCategory::render}).value();
        std::atomic<unsigned> diagnostics=0;
        {
            memory::ThreadContext context{system,heap}; memory::ExecutionScope scope{context,heap};
            graphics::DeviceOptions options; options.validation=validation ? graphics::ValidationMode::required : graphics::ValidationMode::disabled;
            options.ray_tracing=graphics::RayTracingMode::required; options.sampled_image_indexing=graphics::SampledImageIndexingMode::required;
            options.diagnostic_user_data=&diagnostics;
            options.diagnostic_sink=[](void* u,const graphics::Diagnostic& d) noexcept {
                if(d.severity>=VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) { ++*static_cast<std::atomic<unsigned>*>(u); std::fprintf(stderr,"%.*s\n",int(d.message.size()),d.message.data()); }
            };
            auto device=take(graphics::Device::create(heap,options)); report["device"]=device.adapter().properties.deviceName.data(); report["driver"]=device.adapter().driver.driverInfo.data();
            PathPreview worker{system,heap,std::move(device)};
            unsigned sequence=0;
            for(const auto& entry:config.at("sources")) {
                std::ifstream file(entry.at("file").get<std::string>()); Json source; file>>source;
                Camera camera; camera.set_environment(false); camera.set_pathtracing(true);
                const double aspect=double(source.at("width").get<unsigned>())/source.at("height").get<unsigned>();
                auto pose=camera.pose(); const auto& p=source.at("pose");
                for(unsigned i=0;i<3;++i) pose.eye[i]=p.at("eye")[i].get<double>();
                pose.yaw_deg=p.at("yaw"); pose.pitch_deg=p.at("pitch"); pose.focus_distance=p.at("focus");
                check(camera.set_pose(pose,aspect)); auto settings=camera.settings(); settings.lens.automatic_clip=false;
                settings.lens.fov_y=source.at("lens").at("fov_y"); settings.lens.near_plane=source.at("lens").at("near"); settings.lens.far_plane=source.at("lens").at("far");
                check(camera.set_settings(settings,aspect));
                PathPreviewRequest request{*model->snapshot(),camera}; request.read_linear=true;
                request.width=source.at("width"); request.height=source.at("height"); request.settings.direct_only=true;
                request.settings.bounces=1; request.settings.seed=104729+source.at("seed").get<unsigned>(); ++request.settings.reset;
                for(const unsigned target:{256u,512u}) {
                    request.settings.target=target; request.sequence=++sequence; worker.request(request); const auto start=Clock::now();
                    for(;;) {
                        if(auto result=worker.take_result(); result && result->sequence==request.sequence && result->info.samples.count==target) {
                            require(result->info.geometry_fingerprint==source.at("geometry").get<std::uint64_t>(),"Reference geometry differs");
                            auto metadata=media_probe_source(result->info,result->camera);
                            metadata.update({{"motion_source",entry.at("file")},{"exposure_ev",2.5},{"total_ms",elapsed(start)}});
                            const std::string stem=entry.at("name").get<std::string>()+"-ref"+std::to_string(target);
                            save_media_linear(output/stem,result->linear.linear_rgb(),metadata); metadata["stem"]=stem;
                            report["images"].push_back(metadata); std::cout<<stem<<std::endl; break;
                        }
                        const auto status=worker.status(); require(status.error.empty(),status.error.c_str());
                        require(elapsed(start)<180000,"Direct reference timed out"); std::this_thread::sleep_for(1ms);
                    }
                }
            }
        }
        report["diagnostics"]=diagnostics.load(); report["live_allocations"]=heap.snapshot().live_allocations;
        require(!diagnostics && !heap.snapshot().live_allocations,"Reference diagnostics or resources remain");
        require(system.try_close().closed(),"Reference memory close"); report["status"]="passed";
    } catch(const std::exception& e) { report["error"]=e.what(); std::cerr<<e.what()<<'\n'; }
    std::ofstream(output/"report.json")<<report.dump(2); return report["status"]=="passed" ? 0 : 1;
}
