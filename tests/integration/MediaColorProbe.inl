// Frozen cameras and independent seeds: isolate color coverage from motion timing.
int media_color_probe(const std::filesystem::path& root,const std::filesystem::path& output,bool validation) {
    Json report={{"status","failed"},{"validation",validation},{"images",Json::array()}};
    try {
        require(std::filesystem::is_regular_file(root/".dk-editor-smoke"),"Color probe requires a disposable project");
        std::ifstream config_file(root/".dk-media-motion.json"); Json config; config_file>>config; report["config"]=config;
        auto model=take(Workspace::create(root)); check(model->open("project.json"));
        check(model->preset_environment(config.at("preset").get<std::string>())); check(model->apply_environment());
        auto system=memory::MemorySystem::create().value(); auto heap=system.create_heap({"media-color-probe",memory::DomainCategory::render}).value();
        std::atomic<unsigned> diagnostics=0;
        {
            memory::ThreadContext context{system,heap}; memory::ExecutionScope scope{context,heap};
            graphics::DeviceOptions options;
            options.validation=validation ? graphics::ValidationMode::required : graphics::ValidationMode::disabled;
            options.ray_tracing=graphics::RayTracingMode::required; options.sampled_image_indexing=graphics::SampledImageIndexingMode::required;
            options.diagnostic_user_data=&diagnostics;
            options.diagnostic_sink=[](void* u,const graphics::Diagnostic& d) noexcept {
                if(d.severity>=VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
                    ++*static_cast<std::atomic<unsigned>*>(u); std::fprintf(stderr,"%.*s\n",int(d.message.size()),d.message.data());
                }
            };
            auto device=take(graphics::Device::create(heap,options));
            report["device"]=device.adapter().properties.deviceName.data(); report["driver"]=device.adapter().driver.driverInfo.data();
            PathPreview worker{system,heap,std::move(device)};
            Camera camera; camera.set_environment(true); camera.set_pathtracing(true);
            PathPreviewRequest request{*model->snapshot(),camera}; request.read_linear=true;
            request.width=192; request.height=config.value("height",116u);
            request.restir_v1_probe=true; // Compare channel samplers under the same first-surface estimator.
            request.settings.scene_media=true; request.settings.restir_di=config.at("algorithm")=="restir";
            request.settings.restir_temporal=true; request.settings.restir_spatial=true;
            request.settings.time_seconds=17; request.settings.exposure=2.5;
            unsigned pose_index=0;
            for(const double yaw:{85.,95.}) {
                auto pose=camera.pose(); pose.eye={8,1.8,0}; pose.yaw_deg=yaw; pose.pitch_deg=-.9548412539;
                check(camera.set_pose(pose,double(request.width)/request.height)); request.camera=camera;
                for(const unsigned seed:{1u,17u}) for(const std::string name:{"old1","old3","new3","reference96"}) {
                    request.settings.seed=seed; request.settings.target=name=="old1" ? 1 : name=="reference96" ? 96 : 3;
                    request.legacy_media_probe=name.starts_with("old"); ++request.sequence; ++request.settings.reset;
                    const auto start=Clock::now(); worker.request(request);
                    for(;;) {
                        if(auto result=worker.take_result()) {
                            require(result->sequence==request.sequence,"Obsolete color probe image");
                            if(result->info.samples.count==request.settings.target) {
                                auto source=media_probe_source(result->info,result->camera);
                                source.update({{"case",name},{"pose",pose_index},{"exposure_ev",2.5},{"total_ms",elapsed(start)}});
                                const auto stem=name+"-p"+std::to_string(pose_index)+"-s"+std::to_string(seed);
                                save_media_linear(output/stem,result->linear.linear_rgb(),source);
                                source["stem"]=stem; report["images"].push_back(source); std::cout<<stem<<std::endl; break;
                            }
                        }
                        const auto status=worker.status(); require(status.error.empty(),status.error.c_str());
                        require(elapsed(start)<150000,"Color probe timed out"); std::this_thread::sleep_for(1ms);
                    }
                }
                ++pose_index;
            }
        }
        report["diagnostics"]=diagnostics.load(); report["live_allocations"]=heap.snapshot().live_allocations;
        require(!diagnostics && !heap.snapshot().live_allocations,"Color probe diagnostics or resources remain");
        require(system.try_close().closed(),"Color probe memory close"); report["status"]="passed";
    } catch(const std::exception& e) { report["error"]=e.what(); std::cerr<<e.what()<<'\n'; }
    std::ofstream(output/"report.json")<<report.dump(2); return report["status"]=="passed" ? 0 : 1;
}
