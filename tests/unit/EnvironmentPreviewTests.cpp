#include "SceneTestFiles.hpp"
#include "EnvironmentPreview.hpp"
#include <dk/render/DiskScene.hpp>
#include <dk/memory/Context.hpp>
#include <atomic>
using namespace dk;
using namespace dk::editor;
using namespace dk::editor::detail;
using namespace std::chrono_literals;
TEST_CASE("environment worker drops superseded resize cancels and closes active work") {
    SceneTestFiles files;
    std::filesystem::copy(DK_EDITOR_FIXTURE_DIR,files.root,std::filesystem::copy_options::recursive);
    auto model=Workspace::create(files.root).value(); REQUIRE(model->open("project.json"));
    auto* draft=model->environment_draft(); draft->value["enabled"]=true;
    draft->value["atmosphere"]["planet_center_m"][1]=-6361000.0; draft->modified=true;
    REQUIRE(model->apply_environment());
    auto system=memory::MemorySystem::create().value();
    auto heap=system.create_heap({"environment-preview-test",memory::DomainCategory::render}).value();
    std::atomic<unsigned> diagnostics=0;
    graphics::DeviceOptions options; options.validation=graphics::ValidationMode::required;
    options.diagnostic_user_data=&diagnostics;
    options.diagnostic_sink=[](void* user,const graphics::Diagnostic& message) noexcept {
        if(message.severity>=VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
            ++*static_cast<std::atomic<unsigned>*>(user); std::fprintf(stderr,"%.*s\n",int(message.message.size()),message.message.data());
        }
    };
    {
        memory::ThreadContext context{system,heap}; memory::ExecutionScope scope{context,heap};
        Camera camera{true}; camera.set_environment(true);
        {
            auto queue=graphics::SubmissionQueue::create(heap,graphics::Device::create(heap,options).value()).value();
            auto foreign=graphics::SubmissionQueue::create(heap,graphics::Device::create(heap,options).value()).value();
            auto source=render::DiskScene::load_snapshot(heap,model->snapshot()->project,model->snapshot()->scene,{GltfImportProfile::unlit_preview}).value();
            auto assets=source.upload(queue).value();
            auto pipeline=render::ScenePipeline::create(heap,queue,DK_TEST_SHADER_DIR).value();
            const auto view=render::RenderView::create(source.scene(),{32,32,1,camera.world(),camera.projection(1)}).value();
            const auto settings=render::unlit_preview_settings();
            REQUIRE_FALSE(pipeline.render_environment(foreign,view,assets,settings));
            auto frame=pipeline.render_environment(queue,view,assets,settings); REQUIRE(frame); REQUIRE(frame->wait(queue).value());
            REQUIRE_FALSE(pipeline.render_environment(foreign,view,assets,settings));
            auto again=pipeline.render_environment(queue,view,assets,settings); REQUIRE(again); REQUIRE(again->wait(queue).value());
            model->environment_draft()->value["clouds"]["enabled"]=true; model->environment_draft()->modified=true;
            REQUIRE(model->apply_environment());
            auto cloudy=render::RenderScene::extract(heap,model->snapshot()->scene).value();
            const auto cloud_view=render::RenderView::create(cloudy,view.description()).value();
            const auto submitted=queue.stats().submitted;
            const auto rejected=pipeline.render_environment(queue,cloud_view,assets,settings);
#ifdef DK_TEST_CLOUDS
            REQUIRE(rejected); REQUIRE(rejected->wait(queue).value()); CHECK(rejected->cloud_shadow_generation()>0); CHECK(queue.stats().submitted>submitted);
#else
            REQUIRE_FALSE(rejected); CHECK(rejected.error().code==ErrorCode::not_supported);
            CHECK(queue.stats().submitted==submitted);
#endif
            model->environment_draft()->value["clouds"]["enabled"]=false; model->environment_draft()->modified=true;
            REQUIRE(model->apply_environment());
            REQUIRE(queue.close()); REQUIRE(foreign.close());
        }
        EnvironmentPreview worker{system,heap,options};
        const auto request=[&](std::uint64_t seq,unsigned width,unsigned height) { worker.request({*model->snapshot(),camera,width,height,{},seq,1,{}}); };
        const auto wait=[&](auto predicate) {
            const auto end=std::chrono::steady_clock::now()+90s;
            bool ready=predicate();
            while(!ready && std::chrono::steady_clock::now()<end) { std::this_thread::sleep_for(1ms); ready=predicate(); }
            REQUIRE(ready);
        };
        const auto started=[&](std::uint64_t sequence) {
            // A tiny render may finish before the next Windows scheduler tick.
            // Both an active request and its completed result prove it started.
            wait([&] { if(worker.active_sequence()==sequence) return true;
                auto r=worker.take_result(); return r && r->sequence==sequence; });
        };
        const auto result=[&](std::uint64_t sequence) {
            std::optional<EnvironmentPreviewResult> value;
            wait([&] { if(!value) value=worker.take_result(); return value.has_value(); });
            REQUIRE(value->sequence==sequence); INFO(value->error); REQUIRE(value->error.empty()); return std::move(*value);
        };
        request(1,64,64); started(1);
        request(2,96,64); request(3,80,48);
        { auto value=result(3); CHECK(value.info.width==80); CHECK(value.info.height==48); CHECK(value.pixels.size()==80*48*4); }
        CHECK(worker.discarded()>=1);
        // A frozen request survives owner-side changes; the latest generation alone publishes.
        model->environment_draft()->value["sun"]["irradiance"]={2,2,2}; model->environment_draft()->modified=true;
        REQUIRE(model->apply_environment()); request(4,128,64); started(4);
        worker.cancel(); wait([&] { return worker.active_sequence()==0; }); CHECK_FALSE(worker.take_result());
        request(5,40,32); { auto value=result(5); CHECK(value.info.revision==model->snapshot()->state.revision); }
        // A failed view must leave the worker usable.
        camera.set_environment(false); request(6,40,32);
        std::optional<EnvironmentPreviewResult> failed;
        wait([&] { if(!failed) failed=worker.take_result(); return failed.has_value(); });
        CHECK(failed->sequence==6); CHECK_FALSE(failed->error.empty());
        camera.set_environment(true); request(7,40,32); { auto value=result(7); CHECK(value.pixels.size()==40*32*4); }
#ifdef DK_TEST_CLOUDS
        model->environment_draft()->value["clouds"]["enabled"]=true; model->environment_draft()->modified=true;
        REQUIRE(model->apply_environment());
        render::EnvironmentRenderSettings cloud_settings; cloud_settings.mode=render::CloudRenderMode::temporal;
        auto cloud_request=[&](std::uint64_t seq,unsigned width=40,std::optional<TransformEdit> edit={}) {
            worker.request({*model->snapshot(),camera,width,32,edit,seq,1,cloud_settings});
        };
        auto complete=[&](std::uint64_t seq) {
            std::optional<EnvironmentPreviewResult> value;
            wait([&] {
                if(auto update=worker.take_result()) { REQUIRE(update->sequence==seq); REQUIRE(update->error.empty()); value=std::move(update); }
                return value && value->refinement_complete;
            });
            return std::move(*value);
        };
        cloud_request(10); { auto value=complete(10); CHECK_FALSE(value.history_reused); CHECK(value.warmup_frames==32); }
        camera.orbit(.5,0); cloud_request(11);
        { auto value=complete(11); CHECK(value.history_reused); CHECK(value.warmup_frames==8); CHECK(value.history.source_sequence>0); }
        // An explicit camera cut still rejects a completed history.
        cloud_settings.camera_revision=1; cloud_request(12);
        { auto value=complete(12); CHECK_FALSE(value.history_reused); CHECK(value.warmup_frames==32); }
        cloud_request(13,48); { auto value=complete(13); CHECK_FALSE(value.history_reused); CHECK(value.info.width==48); }
        // Cancel during the quiet interval after the first frame; no stale completion survives.
        cloud_request(14,48); { auto value=result(14); CHECK_FALSE(value.refinement_complete); }
        worker.cancel(); wait([&] { return worker.active_sequence()==0; }); CHECK_FALSE(worker.take_result());
        // A rejected request must not consume the cancellation invalidation.
        camera.set_environment(false); cloud_request(15,48);
        wait([&] { auto value=worker.take_result(); return value && !value->error.empty(); });
        camera.set_environment(true); cloud_request(16,48);
        { auto value=complete(16); CHECK_FALSE(value.history_reused); }
        // A detached transform has the same Scene revision but must not reuse its geometry history.
        TransformEdit edit{}; edit.entity=model->snapshot()->scene.entities().front().id;
        edit.value=model->snapshot()->scene.entities().front().local; edit.value.translation.x()+=.25;
        cloud_request(17,48,edit); { auto value=complete(17); CHECK_FALSE(value.history_reused); }
        cloud_request(18,48); { auto value=complete(18); CHECK_FALSE(value.history_reused); }
        camera.orbit(.5,0); cloud_request(19,48); { auto value=complete(19); CHECK(value.history_reused); }
        cloud_request(20,48); { auto value=complete(20); CHECK_FALSE(value.history_reused); CHECK(value.warmup_frames==32); }
        worker.cancel(); camera.orbit(.5,0); cloud_request(21,48);
        { auto value=result(21); CHECK_FALSE(value.refinement_complete); CHECK_FALSE(value.history_reused); }
        camera.orbit(.5,0); cloud_request(22,48);
        { auto value=complete(22); CHECK(value.history_reused); CHECK(value.warmup_frames==32); }
        model->environment_draft()->value["clouds"]["enabled"]=false; model->environment_draft()->modified=true;
        REQUIRE(model->apply_environment());
#endif
        request(23,128,96); started(23);
        // Destruction joins active work before diagnostics, heap and snapshots leave scope.
    }
    CHECK(diagnostics.load()==0); CHECK(heap.snapshot().live_allocations==0); CHECK(system.try_close().closed());
}
