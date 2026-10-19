// Real default-project, default-camera acceptance at desktop extents. No GUI focus needed.
int desktop_path_worker(const std::filesystem::path& root,const std::filesystem::path& output,bool validation) {
    Json report={{"status","failed"},{"validation",validation},{"samples",Json::array()},{"cancel_ms",Json::array()}};
    try {
        auto model=take(Workspace::create(root)); check(model->open("project.json"));
        auto system=memory::MemorySystem::create().value(); auto heap=system.create_heap({"desktop-path-worker",memory::DomainCategory::render}).value();
        std::atomic<unsigned> diagnostics=0;
        {
            memory::ThreadContext context{system,heap}; memory::ExecutionScope scope{context,heap};
            graphics::DeviceOptions options; options.validation=validation ? graphics::ValidationMode::required : graphics::ValidationMode::disabled;
            options.ray_tracing=graphics::RayTracingMode::required; options.sampled_image_indexing=graphics::SampledImageIndexingMode::required;
            options.diagnostic_user_data=&diagnostics;
            options.diagnostic_sink=[](void* u,const graphics::Diagnostic& d) noexcept {
                if(d.severity>=VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
                    ++*static_cast<std::atomic<unsigned>*>(u); std::fprintf(stderr,"%.*s\n",int(d.message.size()),d.message.data());
                }
            };
            auto device=take(graphics::Device::create(heap,options));
            report["device"]=device.adapter().properties.deviceName.data(); report["driver"]=device.adapter().driver.driverInfo.data();
            report["memory_budget_extension"]=device.adapter().memory_budget;
            PathPreview worker{system,heap,std::move(device)};
            Camera camera; camera.set_pathtracing(true);
            PathPreviewRequest request{*model->snapshot(),camera}; request.observe_memory=true;
            auto send=[&] { ++request.sequence; worker.request(request); };
            auto drain=[&] {
                const auto start=Clock::now(); worker.suspend();
                while(worker.status().phase!="Suspended") {
                    require(elapsed(start)<10000,"Desktop cancellation did not drain"); std::this_thread::sleep_for(1ms);
                }
                require(!worker.take_result(),"Suspended desktop worker published"); report["cancel_ms"].push_back(elapsed(start));
            };
            // Both continuation and algorithm changes must fit alongside the last displayed image.
            std::optional<PathPreviewResult> displayed;
            for(const unsigned width:{1600u,2048u}) for(unsigned mode=0;mode<4;++mode) {
                request.width=width; request.height=width==1600 ? 1200 : 2048;
                request.settings.target=width==1600 ? 4 : 2;
                request.settings.restir_di=mode!=0; request.settings.restir_temporal=mode>=2; request.settings.restir_spatial=mode==3;
                const auto start=Clock::now(); auto last=start; unsigned spp=0; send();
                while(spp<request.settings.target) {
                    if(auto result=worker.take_result()) {
                        require(result->sequence==request.sequence && result->info.width==request.width && result->info.height==request.height,"Wrong desktop image identity/extent");
                        require(bool(result->info.restir)==request.settings.restir_di,"Wrong desktop lighting algorithm");
                        if(result->info.restir) require(bool(result->info.restir->temporal)==request.settings.restir_temporal &&
                            bool(result->info.restir->spatial)==request.settings.restir_spatial,"Wrong desktop reuse settings");
                        require(result->memory.owned.peak<=result->memory.owned.limit,"Desktop GPU budget exceeded");
                        const auto now=Clock::now(); spp=result->info.samples.count;
                        Json row={{"width",request.width},{"height",request.height},{"mode",mode},{"spp",spp},
                            {"total_ms",elapsed(start)},{"interval_ms",std::chrono::duration<double,std::milli>(now-last).count()},
                            {"owned_current",result->memory.owned.current},{"owned_peak",result->memory.owned.peak},{"owned_limit",result->memory.owned.limit}};
                        report["samples"].push_back(row); std::cout<<row.dump()<<std::endl;
                        last=now; displayed=std::move(result);
                    }
                    const auto status=worker.status(); require(status.error.empty(),status.error.c_str());
                    require(elapsed(start)<120000,"Desktop path timed out"); std::this_thread::sleep_for(1ms);
                }
                // Cancel an actual in-flight batch and resume from committed samples.
                const auto start_pending=Clock::now(); request.settings.target=64; send();
                while(!worker.status().progress.pending) {
                    require(worker.status().error.empty(),"Desktop continuation failed");
                    require(elapsed(start_pending)<30000,"Desktop pending tile missing"); std::this_thread::sleep_for(1ms);
                }
                drain(); request.settings.target=spp; send();
                const auto resumed=Clock::now();
                for(;;) {
                    if(auto result=worker.take_result()) {
                        require(result->sequence==request.sequence && result->info.samples.count>=spp,"Desktop resume lost complete image");
                        displayed=std::move(result); break;
                    }
                    require(worker.status().error.empty(),"Desktop resume failed");
                    require(elapsed(resumed)<30000,"Desktop resume timed out"); std::this_thread::sleep_for(1ms);
                }
            }
            drain(); report["gpu_memory_limit"]=worker.status().gpu_memory_limit;
        }
        require(!diagnostics && !heap.snapshot().live_allocations,"Desktop worker leaked or emitted diagnostics");
        require(system.try_close().closed(),"Desktop worker memory close"); report["status"]="passed";
    } catch(const std::exception& e) { report["error"]=e.what(); std::cerr<<e.what()<<'\n'; }
    std::ofstream(output/"report.json")<<report.dump(2); return report["status"]=="passed" ? 0 : 1;
}
