#include "EnvironmentPreview.hpp"
#include "PathPreview.hpp"
#include <dk/memory/Context.hpp>
#include <fstream>
#include <iostream>
#include <atomic>
using namespace dk;
using namespace dk::editor;
using namespace dk::editor::detail;
using Clock=std::chrono::steady_clock;
using namespace std::chrono_literals;
namespace {
template<class T> T take(Result<T> r) { if(!r) throw std::runtime_error(r.error().message); return std::move(*r); }
void check(Result<void> r) { if(!r) throw std::runtime_error(r.error().message); }
void require(bool v,const char* message) { if(!v) throw std::runtime_error(message); }
double elapsed(Clock::time_point t) { return std::chrono::duration<double,std::milli>(Clock::now()-t).count(); }
void ppm(const std::filesystem::path& path,const EnvironmentPreviewResult& r) {
    std::ofstream f(path,std::ios::binary); f<<"P6\n"<<r.info.width<<' '<<r.info.height<<"\n255\n";
    for(std::size_t i=0;i<r.pixels.size();i+=4) f.write(reinterpret_cast<const char*>(r.pixels.data()+i),3);
    require(bool(f),"Cannot write pixels");
}
}
#include "RestirWorker.inl"
int main(int argc,char** argv) {
    if(argc<3) return 2;
    bool validation=false,profile=false,media=false,restir=false;
    for(int i=3;i<argc;++i) {
        const std::string_view option=argv[i];
        if(option=="--validation") validation=true; else if(option=="--profile") profile=true;
        else if(option=="--media") media=true; else if(option=="--restir") restir=true; else return 2;
    }
    const std::filesystem::path output=argv[2]; std::filesystem::create_directories(output);
    if(restir) return restir_worker(argv[1],output);
    Json report={{"status","failed"},{"validation",validation},{"profile",profile},{"environment",Json::array()},{"path",Json::array()}};
    int code=1;
    try {
        auto model=take(Workspace::create(argv[1])); check(model->open("project.json"));
        auto system=memory::MemorySystem::create().value();
        auto heap=system.create_heap({"editor-render-benchmark",memory::DomainCategory::render}).value();
        std::atomic<unsigned> diagnostics=0;
        graphics::DeviceOptions options; options.validation=validation ? graphics::ValidationMode::required : graphics::ValidationMode::disabled;
        options.diagnostic_user_data=&diagnostics;
        options.diagnostic_sink=[](void* u,const graphics::Diagnostic& d) noexcept {
            if(d.severity>=VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
                ++*static_cast<std::atomic<unsigned>*>(u); std::fprintf(stderr,"%.*s\n",int(d.message.size()),d.message.data());
            }
        };
        const unsigned width=validation ? 320 : 1280,height=validation ? 180 : 720;
        report["width"]=media ? 192 : width; report["height"]=media ? 108 : height;
        if(media) report.update({{"model",4},{"sampler",2},{"seed",1},{"time",17},{"bounces",4}});
        {
            memory::ThreadContext context{system,heap}; memory::ExecutionScope scope{context,heap};
            Camera camera; camera.set_environment(true);
            if(!media) {
                EnvironmentPreview worker{system,heap,options,profile}; std::uint64_t sequence=0;
                for(const char* name:{"clear","sparse","overcast","sky-clear","sky-overcast"}) {
                    const bool sky_view=std::string_view{name}.starts_with("sky-");
                    const char* preset=sky_view ? name+4 : name;
                    check(model->preset_environment(preset)); check(model->apply_environment());
                    const bool cloudy=std::string_view{preset}!="clear";
                    for(unsigned frame=0;frame<(sky_view ? 1u : validation ? 2u : 4u);++frame) {
                        if(frame) camera.orbit(.5,0);
                        Camera render_camera=camera;
                        if(sky_view) { render_camera.pan(0,2000.*height/720,height); render_camera.orbit(0,-160); }
                        render::EnvironmentRenderSettings settings; settings.mode=render::CloudRenderMode::temporal;
                        settings.camera_revision=render_camera.revision();
                        const auto start=Clock::now();
                        worker.request({*model->snapshot(),render_camera,width,height,{},++sequence,1,settings});
                        bool first=true; Json sample={{"preset",name},{"frame",frame},{"publications",0}};
                        for(;;) {
                            if(auto result=worker.take_result()) {
                                require(result->sequence==sequence,"Obsolete environment result published");
                                if(!result->error.empty()) throw std::runtime_error(result->error);
                                require(result->pixels.size()==std::size_t(width)*height*4,"Incomplete image published");
                                if(first) { sample["first_ms"]=elapsed(start); first=false; }
                                sample["publications"]=sample["publications"].get<unsigned>()+1;
                                if(!cloudy || result->warmup_frames==32) {
                                    sample["complete_ms"]=elapsed(start); sample["warmup_frames"]=result->warmup_frames;
                                    if(profile) {
                                        require(result->gpu_profile.status()==graphics::GpuProfileStatus::ready && !result->gpu_profile.dropped_zones(),"Missing GPU profile");
                                        sample["frame_gpu"]=Json::array();
                                        for(const auto& zone:result->gpu_profile.timings()) sample["frame_gpu"].push_back({{"name",zone.name},{"ms",zone.elapsed_ns/1e6}});
                                        sample["device_local_allocation_bytes"]=result->device_local_bytes;
                                    }
                                    if(frame==0) ppm(output/(std::string{name}+".ppm"),*result);
                                    break;
                                }
                            }
                            require(elapsed(start)<120000,"Environment worker timed out"); std::this_thread::sleep_for(1ms);
                        }
                        report["environment"].push_back(sample); std::cout<<sample.dump()<<std::endl;
                    }
                }
                // Directly exercise a normal viewport's reference-mode rejection and recovery.
                render::EnvironmentRenderSettings settings; settings.mode=render::CloudRenderMode::reference;
                worker.request({*model->snapshot(),camera,width,height,{},++sequence,1,settings});
                const auto start=Clock::now();
                for(;;) {
                    if(auto r=worker.take_result()) { require(!r->error.empty(),"Oversize reference unexpectedly accepted"); report["reference_error"]=r->error; break; }
                    require(elapsed(start)<120000,"Reference error timed out"); std::this_thread::sleep_for(1ms);
                }
            }
            check(model->preset_environment("clear")); check(model->apply_environment());
            if(!media) {
                options.ray_tracing=graphics::RayTracingMode::required;
                options.sampled_image_indexing=graphics::SampledImageIndexingMode::required;
                auto device=take(graphics::Device::create(heap,options));
                report["device"]=std::string{device.physical_device().getProperties().deviceName.data()};
                PathPreview worker{system,heap,std::move(device)};
                PathPreviewRequest request{*model->snapshot(),camera};
                request.width=width; request.height=height; request.sequence=1; request.settings.target=6; request.settings.surface_only=true;
                request.observe_memory=profile;
                const auto start=Clock::now(); auto last=start; worker.request(request); unsigned spp=0;
                while(spp<request.settings.target) {
                    if(auto r=worker.take_result()) {
                        require(r->sequence==1,"Obsolete path result published");
                        const auto now=Clock::now(); spp=r->info.samples.count;
                        Json sample={{"spp",spp},{"total_ms",elapsed(start)},{"interval_ms",std::chrono::duration<double,std::milli>(now-last).count()}};
                        if(profile) sample["device_allocation_bytes"]=r->memory.device_allocation_bytes;
                        last=now; report["path"].push_back(sample); std::cout<<sample.dump()<<std::endl;
                    }
                    const auto status=worker.status(); if(!status.error.empty()) throw std::runtime_error(status.error);
                    require(elapsed(start)<120000,"Path worker timed out"); std::this_thread::sleep_for(1ms);
                }
            }
            if(media) {
                options.ray_tracing=graphics::RayTracingMode::required;
                options.sampled_image_indexing=graphics::SampledImageIndexingMode::required;
                const auto device_start=Clock::now();
                auto device=take(graphics::Device::create(heap,options));
                report["device_ms"]=elapsed(device_start);
                report["device"]=device.adapter().properties.deviceName.data();
                PathPreview worker{system,heap,std::move(device)}; std::uint64_t sequence=0;
                report["media"]=Json::array();
                for(const char* preset:{"clear","sparse","overcast"}) {
                    check(model->preset_environment(preset)); check(model->apply_environment());
                    const auto world=camera.world().matrix();
                    const Vec3d eye=world.block<3,1>(0,3),target=eye-world.block<3,1>(0,2);
                    Json item={{"preset",preset},{"camera_ms",Json::array()},{"cancel_ms",Json::array()},
                        {"eye",{eye.x(),eye.y(),eye.z()}},{"target",{target.x(),target.y(),target.z()}},
                        {"environment",take(model->execute_console(R"({"method":"environment.query","params":{}})"))["environment"]}};
                    for(unsigned i=0;i<13;++i) {
                        if(i) camera.orbit(.5,0);
                        PathPreviewRequest request{*model->snapshot(),camera};
                        request.width=192; request.height=108; request.sequence=++sequence;
                        request.settings.target=64; request.settings.scene_media=true; request.settings.time_seconds=17;
                        request.observe_memory=true;
                        const auto start=Clock::now(); worker.request(request);
                        for(;;) {
                            if(auto result=worker.take_result()) {
                                require(result->sequence==sequence && result->camera.revision()==camera.revision(),"Obsolete media camera");
                                require(result->info.medium && result->info.width==192 && result->info.height==108,"Wrong media input");
                                require(result->memory.owned.peak<=result->memory.owned.limit,"Worker GPU budget exceeded");
                                item["owned_peak"]=result->memory.owned.peak;
                                if(i) item["camera_ms"].push_back(elapsed(start)); else item["first_ms"]=elapsed(start);
                                break;
                            }
                            const auto status=worker.status(); require(status.error.empty(),status.error.c_str());
                            require(elapsed(start)<120000,"Media worker timeout"); std::this_thread::sleep_for(1ms);
                        }
                        const auto pending=Clock::now();
                        while(!worker.status().progress.pending) {
                            require(elapsed(pending)<30000,"Missing in-flight media tile"); std::this_thread::sleep_for(1ms);
                        }
                        const auto cancel=Clock::now(); worker.suspend();
                        while(worker.status().phase!="Suspended") {
                            require(elapsed(cancel)<10000,"Media cancellation did not drain"); std::this_thread::sleep_for(1ms);
                        }
                        item["cancel_ms"].push_back(elapsed(cancel));
                        require(!worker.take_result(),"Late publication after suspend");
                    }
                    report["media"].push_back(item); std::cout<<item.dump()<<std::endl;
                }
            }
        }
        report["diagnostics"]=diagnostics.load(); report["live_allocations"]=heap.snapshot().live_allocations;
        report["cpu_heap_peak_backing_bytes"]=heap.snapshot().peak_backing_bytes;
        require(!diagnostics && !heap.snapshot().live_allocations,"Diagnostics or live resources after worker close");
        require(system.try_close().closed(),"Memory system did not close"); report["status"]="passed"; code=0;
    } catch(const std::exception& e) { report["error"]=e.what(); std::cerr<<e.what()<<'\n'; }
    std::ofstream(output/"report.json")<<report.dump(2); return code;
}
