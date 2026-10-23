#include <cmath>
// Consumer lifecycle checks. GPU transport math is covered by PathTraceGpuTests.
int restir_worker(const std::filesystem::path& root,const std::filesystem::path& output) {
    Json evidence={{"status","failed"},{"checks",Json::array()}};
    try {
        auto model=take(Workspace::create(root)); check(model->open("project.json"));
        auto system=memory::MemorySystem::create().value(); auto heap=system.create_heap({"restir-worker",memory::DomainCategory::render}).value();
        std::atomic<unsigned> diagnostics=0;
        {
            memory::ThreadContext context{system,heap}; memory::ExecutionScope scope{context,heap};
            graphics::DeviceOptions options; options.validation=graphics::ValidationMode::required; options.secondary_queue=true;
            options.ray_tracing=graphics::RayTracingMode::required; options.sampled_image_indexing=graphics::SampledImageIndexingMode::required;
            options.diagnostic_user_data=&diagnostics;
            options.diagnostic_sink=[](void* u,const graphics::Diagnostic& d) noexcept {
                if(d.severity>=VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) ++*static_cast<std::atomic<unsigned>*>(u);
            };
            auto device=take(graphics::Device::create(heap,options));
            auto consumer=take(graphics::SubmissionQueue::create(heap,take(device.share_queue(0))));
            graphics::Image retained;
            std::vector<std::byte> retained_pixels;
            auto pixels=[&](const graphics::Image& image) {
                const auto& d=image.description();
                auto batch=take(consumer.begin());
                auto readback=take(batch.readback(image,{0,0,0,0,d.width,d.height}));
                check(batch.transition(image,vk::ImageLayout::eShaderReadOnlyOptimal));
                require(take(consumer.wait(take(consumer.submit(std::move(batch))))),"Worker image inspection timed out");
                const auto copy=readback.description(); std::vector<std::byte> source(std::size_t(copy.bytes));
                require(take(readback.try_read(source)),"Worker image inspection incomplete");
                std::vector<std::byte> result(std::size_t(d.width)*d.height*4);
                for(unsigned y=0;y<d.height;++y)
                    std::copy_n(source.begin()+std::size_t(y)*copy.row_pitch,std::size_t(d.width)*4,result.begin()+std::size_t(y)*d.width*4);
                return result;
            };
            auto producer=std::make_unique<PathPreview>(system,heap,take(device.share_queue(device.queue_count()>1 ? 1u : 0u)));
            auto& worker=*producer;
            PathPreviewRequest request{*model->snapshot(),Camera{true}};
            request.width=32; request.height=24; request.read_linear=true;
            request.settings.target=3; request.settings.restir_di=request.settings.restir_temporal=request.settings.restir_spatial=true;
            auto send=[&] { ++request.sequence; worker.request(request); };
            auto wait=[&](unsigned spp) {
                const auto start=Clock::now();
                for(;;) {
                    if(auto r=worker.take_result()) {
                        require(r->sequence==request.sequence,"Obsolete worker result");
                        if(!spp || r->info.samples.count==spp) return std::move(*r);
                    }
                    const auto status=worker.status(); require(status.error.empty(),status.error.c_str());
                    require(elapsed(start)<90000,"ReSTIR worker timed out"); std::this_thread::sleep_for(1ms);
                }
            };
            send(); auto first=wait(3); const auto generation=worker.status().progress.generation;
            require(first.info.restir && first.info.restir->temporal && first.info.restir->spatial,"Worker omitted ReSTIR settings");
            require(first.info.sampling_first_epoch==0 && !first.info.temporal_history_epoch,"Initial view not empty");
            request.settings.paused=true; request.settings.target=4; request.settings.exposure=2; send(); auto paused=wait(3);
            require(worker.status().progress.generation==generation,"Display controls reset history");
            require(std::equal(first.linear.linear_rgb().begin(),first.linear.linear_rgb().end(),paused.linear.linear_rgb().begin()),"Exposure changed linear history");
            request.settings.reset++; send();
            auto start=Clock::now();
            while(worker.status().phase!="Paused" || worker.status().progress.completed_spp!=0) {
                require(elapsed(start)<90000,"Paused reset did not clear history"); std::this_thread::sleep_for(1ms);
            }
            require(worker.status().reset_reason=="Manual reset","Missing reset diagnostic");
            request.settings.paused=false; request.settings.target=3; send(); auto reset=wait(3);
            require(std::equal(first.linear.linear_rgb().begin(),first.linear.linear_rgb().end(),reset.linear.linear_rgb().begin()),"Reset not reproducible");
            request.camera.orbit(.1,0); send(); auto moved=wait(3);
            require(moved.info.sampling_first_epoch==3 && moved.info.temporal_history_epoch.has_value(),"Pose lost temporal owner");
            require(worker.status().reset_reason=="Camera pose changed","Wrong camera reset reason");
            evidence["checks"].push_back("pause/target/exposure preserve; manual reset reproduces; camera preserves sampling history");
            request.reload++; send(); auto refreshed=wait(3);
            require(refreshed.info.sampling_first_epoch==0 && !refreshed.info.temporal_history_epoch,"Refresh retained history");
            request.width=40; send(); auto resized=wait(3);
            require(resized.info.sampling_first_epoch==0 && resized.info.width==40,"Resize retained history");
            request.settings.restir_candidates=4; send(); auto changed=wait(3);
            require(changed.info.restir->initial_candidates==4 && !changed.info.temporal_history_epoch,"Candidates not applied");
            request.settings.restir_di=false; send(); auto classic=wait(3); require(!classic.info.restir,"Classic switch retained reservoirs");
            request.settings.restir_di=true; send(); auto restored=wait(3);
            require(restored.info.sampling_first_epoch==0 && !restored.info.temporal_history_epoch,"Algorithm switch retained history");
            request.settings.restir_candidates=0; send(); start=Clock::now();
            while(worker.status().error.empty()) { require(elapsed(start)<30000,"Invalid worker configuration accepted"); std::this_thread::sleep_for(1ms); }
            require(!worker.take_result(),"Failed configuration published output");
            request.settings.restir_candidates=4; send(); auto recovered=wait(3);
            require(std::equal(restored.linear.linear_rgb().begin(),restored.linear.linear_rgb().end(),recovered.linear.linear_rgb().begin()),"Configuration failure changed history");
            evidence["checks"].push_back("refresh/resize/candidates/algorithm reset; failed config preserves committed image");
            request.settings.target=4096; send(); start=Clock::now();
            while(!worker.status().progress.pending) { require(elapsed(start)<90000,"No pending worker epoch"); std::this_thread::sleep_for(1ms); }
            worker.suspend(); start=Clock::now();
            while(worker.status().phase!="Suspended") { require(elapsed(start)<10000,"Cancel did not drain"); std::this_thread::sleep_for(1ms); }
            require(!worker.take_result(),"Suspended worker published late result");
            request.settings.target=3; request.settings.reset++; send(); auto returned=wait(3);
            require(returned.info.sampling_first_epoch==0 && !returned.info.temporal_history_epoch,"Resume retained cancelled history");
            // Tone and acceptance readback share three producer slots with the
            // next sampling submission. Check the image against its own HDR while
            // that producer continues, then retain it beyond producer destruction.
            request.settings.target=4096; send();
            auto progressing=wait(0);
            require(progressing.info.samples.count>=returned.info.samples.count && progressing.info.samples.count<=request.settings.target,
                "Continuation published an incomplete sample interval");
            require(progressing.linear.info().samples.first==progressing.info.samples.first &&
                progressing.linear.info().samples.count==progressing.info.samples.count,"Tone/readback sources differ");
            retained=take(consumer.import_image(std::move(progressing.image))); retained_pixels=pixels(retained);
            const auto colors=progressing.linear.linear_rgb();
            require(retained_pixels.size()==colors.size()*4,"Worker tone extent differs from HDR");
            for(std::size_t i=0;i<colors.size();++i) {
                for(unsigned c=0;c<3;++c) {
                    const double linear=colors[i][c]*std::exp2(request.settings.exposure);
                    double expected=linear/(1+linear);
                    if(!request.srgb) expected=expected<=.0031308 ? 12.92*expected : 1.055*std::pow(expected,1./2.4)-.055;
                    require(std::abs(int(std::to_integer<unsigned char>(retained_pixels[i*4+c]))-std::lround(expected*255))<=1,
                        "Overlapped tone differs from its completed HDR");
                }
                require(retained_pixels[i*4+3]==std::byte{255},"Worker tone alpha differs");
            }
            start=Clock::now();
            while(!worker.status().progress.pending) {
                require(worker.status().error.empty(),"Worker continuation failed");
                require(elapsed(start)<90000,"No live producer at destruction"); std::this_thread::sleep_for(1ms);
            }
            // Destructor joins this live producer without caller-side wait/close.
            evidence["checks"].push_back("three-slot tone/readback/continuation; image pixels match their completed HDR");
            producer.reset();
            require(pixels(retained)==retained_pixels,"Producer destruction changed its transferred image");
            retained={};
            // The same-device single native queue fallback still has independent
            // consumer/producer ledgers and the same image handoff dependency.
            {
                PathPreview fallback{system,heap,take(device.share_queue(0))};
                PathPreviewRequest request{*model->snapshot(),Camera{true}};
                request.width=8; request.height=6; request.sequence=1; request.settings.target=2;
                fallback.request(request); const auto start=Clock::now();
                for(;;) {
                    if(auto result=fallback.take_result(); result && result->info.samples.count==request.settings.target) {
                        require(result->sequence==request.sequence,"Fallback published obsolete image");
                        auto image=take(consumer.import_image(std::move(result->image)));
                        require(pixels(image).size()==std::size_t(request.width)*request.height*4,"Fallback image handoff incomplete"); break;
                    }
                    require(fallback.status().error.empty(),"Single native queue worker failed");
                    require(elapsed(start)<90000,"Single native queue worker timed out"); std::this_thread::sleep_for(1ms);
                }
            }
            check(consumer.close());
            evidence["checks"].push_back("in-flight suspend/recovery; transferred image survives live producer destruction; single native queue fallback");
            evidence["queues"]=device.queue_count();
        }
        require(!diagnostics && !heap.snapshot().live_allocations,"Worker leaked resources or emitted validation diagnostics");
        require(system.try_close().closed(),"Worker memory did not close"); evidence["status"]="passed";
    } catch(const std::exception& e) { evidence["error"]=e.what(); std::cerr<<e.what()<<'\n'; }
    std::ofstream(output/"report.json")<<evidence.dump(2); return evidence["status"]=="passed" ? 0 : 1;
}
