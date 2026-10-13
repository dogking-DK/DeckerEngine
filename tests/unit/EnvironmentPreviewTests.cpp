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
            while(!predicate() && std::chrono::steady_clock::now()<end) std::this_thread::sleep_for(1ms);
            REQUIRE(predicate());
        };
        const auto result=[&](std::uint64_t sequence) {
            std::optional<EnvironmentPreviewResult> value;
            wait([&] { if(!value) value=worker.take_result(); return value.has_value(); });
            REQUIRE(value->sequence==sequence); INFO(value->error); REQUIRE(value->error.empty()); return std::move(*value);
        };
        request(1,64,64); wait([&] { return worker.active_sequence()==1; });
        request(2,96,64); request(3,80,48);
        { auto value=result(3); CHECK(value.info.width==80); CHECK(value.info.height==48); CHECK(value.pixels.size()==80*48*4); }
        CHECK(worker.discarded()>=1);
        // A frozen request survives owner-side changes; the latest generation alone publishes.
        model->environment_draft()->value["sun"]["irradiance"]={2,2,2}; model->environment_draft()->modified=true;
        REQUIRE(model->apply_environment()); request(4,128,64); wait([&] { return worker.active_sequence()==4; });
        worker.cancel(); wait([&] { return worker.active_sequence()==0; }); CHECK_FALSE(worker.take_result());
        request(5,40,32); { auto value=result(5); CHECK(value.info.revision==model->snapshot()->state.revision); }
        // A failed view must leave the worker usable.
        camera.set_environment(false); request(6,40,32);
        std::optional<EnvironmentPreviewResult> failed;
        wait([&] { if(!failed) failed=worker.take_result(); return failed.has_value(); });
        CHECK(failed->sequence==6); CHECK_FALSE(failed->error.empty());
        camera.set_environment(true); request(7,40,32); { auto value=result(7); CHECK(value.pixels.size()==40*32*4); }
        request(8,128,96); wait([&] { return worker.active_sequence()==8; });
        // Destruction joins active work before diagnostics, heap and snapshots leave scope.
    }
    CHECK(diagnostics.load()==0); CHECK(heap.snapshot().live_allocations==0); CHECK(system.try_close().closed());
}
