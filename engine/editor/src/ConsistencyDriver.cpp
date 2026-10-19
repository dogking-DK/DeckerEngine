#include "ConsistencyDriver.hpp"
#include "EditorSupport.hpp"
#include <dk/io/File.hpp>
#include <array>
#include <cstdio>

namespace dk::editor::detail {
namespace {
constexpr std::array names={"initial","gui","external","draft","conflict","undo","redo","saved","reloaded","gizmo","orthographic","ortho_height","perspective"};
void require(bool value,const char* message) {
    if (!value) throw std::runtime_error{std::string{"Consistency smoke: "}+message};
}
std::span<const char* const> actions(std::size_t stage,bool environment,bool clouds) {
    static constexpr std::array lens={"camera_settings","camera_fov",":all",":fov",":enter","camera_apply"};
    if(stage==0) return lens;
    static constexpr std::array gui={"viewport","name",":all",":name","translation",":all",":translation","apply"};
    static constexpr std::array draft={"name",":all",":draft"};
    static constexpr std::array conflict={"apply","revert"};
    static constexpr std::array undo={"undo"},redo={"redo"},save={"save"},reload={"reload"};
    if(environment) {
        static constexpr std::array ortho={"camera_orthographic"},perspective={"camera_perspective"};
        static constexpr std::array height={"camera_settings","camera_ortho_height",":all",":height",":enter","camera_apply"};
        if(stage==10) return ortho; if(stage==11) return height; if(stage==12) return perspective;
        static constexpr std::array env_gui={"environment_open","environment_enabled","environment_apply"};
        static constexpr std::array env_draft={"environment_open","environment_enabled"};
        static constexpr std::array env_conflict={"environment_apply","environment_revert"};
        static constexpr std::array env_gizmo={"viewport","gizmo_0",":drag",":release"};
        static constexpr std::array cloud_gizmo={"entity","camera_frame","gizmo_0",":drag",":release"};
        if(stage==9) return clouds ? std::span<const char* const>{cloud_gizmo} : std::span<const char* const>{env_gizmo};
        static constexpr std::array cloud_gui={"environment_open","cloud_preset_sparse","environment_apply"};
        if(stage==1) return clouds ? std::span<const char* const>{cloud_gui} : std::span<const char* const>{env_gui};
        if(stage==3) return env_draft;
        if(stage==4) return env_conflict;
    }
    switch (stage) {
    case 1: return gui;
    case 3: return draft;
    case 4: return conflict;
    case 5: return undo;
    case 6: return redo;
    case 7: return save;
    case 8: return reload;
    default: return {};
    }
}
Json vector(const Vec3d& v) { return Json::array({v.x(),v.y(),v.z()}); }
Json entity(const EntityData& value) {
    const auto& t=value.local;
    Json assets=Json::array();
    for (const auto& a:value.assets) assets.push_back({{"id",a.id.to_string()},{"kind",asset_kind_name(a.kind)}});
    return {{"id",value.id.to_string()},{"name",value.name},{"parent",value.parent ? Json(value.parent->to_string()) : Json(nullptr)},
        {"assets",assets},{"transform",{{"translation",vector(t.translation)},{"rotation",{t.rotation.x(),t.rotation.y(),t.rotation.z(),t.rotation.w()}},{"scale",vector(t.scale)}}}};
}
Json camera(const Camera& value) {
    const Mat4d world=value.world().matrix();
    const Vec3d eye=world.block<3,1>(0,3),target=eye-world.block<3,1>(0,2),up=world.block<3,1>(0,1);
    Json result={{"eye",vector(eye)},{"target",vector(target)},{"up",vector(up)},{"near",value.near_plane()},{"far",value.far_plane()}};
    const bool ortho=value.projection_mode()==ProjectionMode::orthographic; result["projection"]=ortho ? "orthographic" : "perspective";
    if(ortho) result["ortho_height"]=value.settings().lens.ortho_height; else result["fov_y"]=value.fov_y();
    return result;
}
}
ConsistencyDriver::ConsistencyDriver(const std::filesystem::path& root,bool environment) : directory_(root/(environment ? ".dk-environment" : ".dk-consistency")),environment_(environment),clouds_(std::filesystem::is_regular_file(root/".dk-cloud-integration")) {
    require(std::filesystem::create_directory(directory_),"checkpoint directory must be fresh");
}
void ConsistencyDriver::input(WorkbenchUi& ui,const Workspace& model,const Viewport& viewport) {
    if (done_) return;
    if (waiting_) {
        if (!std::filesystem::is_regular_file(directory_/(std::string{names[stage_]}+".continue"))) return;
        waiting_=false; ready_=false; tick_=0;
        if (++stage_==(environment_ ? names.size() : 9)) { done_=true; return; }
    }
    if(environment_ && stage_==9 && tick_==0) {
        if(viewport.error().empty() && !viewport.current(model.snapshot()->state)) return;
        gizmo_revision_=model.snapshot()->state.revision;
    }
    auto& io=ImGui::GetIO(); io.ConfigInputTrickleEventQueue=false;
    if (tick_++<12) return;
    const auto sequence=actions(stage_,environment_,clouds_);
    const auto elapsed=tick_-13,step=elapsed/8,phase=elapsed%8;
    if (step>=sequence.size()) { ready_=true; return; }
    const std::string_view action=sequence[step];
    if(environment_ && stage_==9 && action=="gizmo_0" && phase==0 && viewport.error().empty() &&
        (!viewport.current(model.snapshot()->state) || !viewport.camera_current(ui.input().camera()))) { --tick_; return; }
    if (phase==0) {
        io.AddFocusEvent(true);
        if (action==":all") { io.AddKeyEvent(ImGuiMod_Ctrl,true); io.AddKeyEvent(ImGuiKey_A,true); }
        else if (action==":drag") pointer_.x+=40;
        else if (action==":release") io.AddMouseButtonEvent(0,false);
        else if (action==":name") io.AddInputCharactersUTF8("GUI consistency");
        else if (action==":height") io.AddInputCharactersUTF8("3");
        else if (action==":fov") io.AddInputCharactersUTF8("55");
        else if (action==":enter") io.AddKeyEvent(ImGuiKey_Enter,true);
        else if (action==":translation") io.AddInputCharactersUTF8("0.25");
        else if (action==":draft") io.AddInputCharactersUTF8("Stale GUI draft");
        else {
            const auto control=ui.controls().find(std::string{action});
            require(control!=ui.controls().end(),"missing GUI control");
            pointer_=control->second; io.AddMousePosEvent(pointer_.x,pointer_.y); io.AddMouseButtonEvent(0,true);
        }
    }
    io.AddMousePosEvent(pointer_.x,pointer_.y);
    if (phase==1) {
        if (action==":all") { io.AddKeyEvent(ImGuiKey_A,false); io.AddKeyEvent(ImGuiMod_Ctrl,false); }
        else if(action==":enter") io.AddKeyEvent(ImGuiKey_Enter,false);
        else if (!action.starts_with(":") && !(environment_ && stage_==9 && action=="gizmo_0")) io.AddMouseButtonEvent(0,false);
    }
}
void ConsistencyDriver::verify(const Workspace& model,const WorkbenchUi& ui,const Viewport& viewport) const {
    if(environment_) { verify_environment(model,ui,viewport); return; }
    const auto* snapshot=model.snapshot();
    require(snapshot && !ui.closing() && viewport.texture() && viewport.error().empty(),"missing scene or valid viewport");
    require(!ui.input().active() && viewport.current(snapshot->state) && viewport.camera_current(ui.input().camera()),"viewport is stale or contains an uncommitted preview");
    require(ui.errors()==(stage_>=4 ? 1u : 0u),"unexpected GUI command/preview errors");
    const auto revision=snapshot->state.revision,pixels=viewport.pixel_signature();
    const auto history=model.history();
    const auto prior_revision=[&](std::size_t i) { return checkpoints_[i]["state"]["revision"].get<std::uint64_t>(); };
    const auto prior_pixels=[&](std::size_t i) { return checkpoints_[i]["viewport"]["rgba_signature"].get<std::uint64_t>(); };
    if (stage_>0 && stage_<8) require(model.draft() && model.selection(),"selection/Inspector was lost");
    switch (stage_) {
    case 0: require(!model.dirty(),"fixture must start saved"); break;
    case 1:
        require(model.draft()->entity.name=="GUI consistency" && model.draft()->entity.local.translation.x()==0.25 &&
            !model.pending() && revision==prior_revision(0)+1 && history.undo_count==1 && pixels!=prior_pixels(0),"GUI Apply did not publish exactly one visible transaction"); break;
    case 2:
        require(model.draft()->entity.name=="External consistency" && revision==prior_revision(1)+1 && history.undo_count==2 &&
            pixels!=prior_pixels(1),"external transaction was not reflected in Inspector/viewport"); break;
    case 3:
        require(model.pending() && model.draft()->entity.name=="Stale GUI draft" && revision==prior_revision(2) &&
            pixels==prior_pixels(2) && history.undo_count==2,"draft changed committed scene or viewport"); break;
    case 4:
        require(!model.pending() && model.draft()->entity.name=="External during draft" && revision==prior_revision(3)+1 &&
            history.undo_count==3 && pixels!=prior_pixels(3),"stale Apply overwrote remote edit or Revert failed"); break;
    case 5: require(revision==prior_revision(4)+1 && history.undo_count==2 && history.redo_count==1 && pixels==prior_pixels(2),"GUI Undo did not restore external content/image"); break;
    case 6: require(revision==prior_revision(5)+1 && history.undo_count==3 && history.redo_count==0 && pixels==prior_pixels(4),"GUI Redo did not restore external edit/image"); break;
    case 7: require(!model.dirty() && revision==prior_revision(6) && pixels==prior_pixels(6),"GUI Save changed content or left dirty state"); break;
    case 8:
        require(snapshot->state.document_id.to_string()!=checkpoints_[7]["state"]["document_id"].get<std::string>() &&
            !model.selection() && !model.draft() && history.undo_count==0 && !model.dirty() && revision==prior_revision(7) &&
            pixels==prior_pixels(7),"Reload changed persisted content/image or retained old session"); break;
    }
}
void ConsistencyDriver::verify_environment(const Workspace& model,const WorkbenchUi& ui,const Viewport& viewport) const {
    const auto* snapshot=model.snapshot();
    require(snapshot && !ui.closing() && viewport.texture() && viewport.error().empty(),"missing environment viewport");
    require(!ui.input().active() && viewport.current(snapshot->state) && viewport.camera_current(ui.input().camera()),"stale environment viewport");
    require(ui.errors()==(stage_>=4 ? 1u : 0u),"unexpected environment command errors");
    const auto revision=snapshot->state.revision,pixels=viewport.pixel_signature(); const auto history=model.history();
    const auto rev=[&](std::size_t i) { return checkpoints_[i]["state"]["revision"].get<std::uint64_t>(); };
    const auto sig=[&](std::size_t i) { return checkpoints_[i]["viewport"]["rgba_signature"].get<std::uint64_t>(); };
    switch(stage_) {
    case 0: require(!model.dirty() && !snapshot->scene.environment().enabled,"environment fixture must start saved/disabled"); break;
    case 1: require(snapshot->scene.environment().enabled && !model.pending() && revision==rev(0)+1 && history.undo_count==1 && pixels!=sig(0),"GUI environment Apply failed"); break;
    case 2: require(!model.pending() && revision==rev(1)+1 && history.undo_count==2 && pixels!=sig(1),"external environment not visible"); break;
    case 3: require(model.pending() && !model.environment_draft()->value["enabled"].get<bool>() && revision==rev(2) && pixels==sig(2),"environment draft changed committed frame"); break;
    case 4: require(!model.pending() && model.environment_draft()->value["enabled"].get<bool>() && revision==rev(3)+1 && history.undo_count==3 && pixels!=sig(3),"stale environment Apply or Revert failed"); break;
    case 5: require(revision==rev(4)+1 && history.undo_count==2 && history.redo_count==1 && pixels==sig(2),"environment Undo failed"); break;
    case 6: require(revision==rev(5)+1 && history.undo_count==3 && history.redo_count==0 && pixels==sig(4),"environment Redo failed"); break;
    case 7: require(!model.dirty() && revision==rev(6) && pixels==sig(6),"environment Save failed"); break;
    case 10: case 11: case 12:
        require(revision==rev(stage_-1) && pixels!=sig(stage_-1),"projection/height did not change only image");
        require(ui.input().camera().projection_mode()==(stage_==12 ? ProjectionMode::perspective : ProjectionMode::orthographic),"environment projection control failed");
        if(stage_==11) require(ui.input().camera().settings().lens.ortho_height==3,"environment orthographic height failed");
        break;
    case 9: require(revision==gizmo_revision_+1 && !model.pending() && model.draft() && model.draft()->entity.local.translation.x()!=0,"asynchronous environment Gizmo did not commit"); break;
    case 8: require(snapshot->state.document_id.to_string()!=checkpoints_[7]["state"]["document_id"].get<std::string>() && !model.dirty() && !model.pending() && history.undo_count==0 && revision==rev(7) && pixels==sig(7),"environment Reload failed"); break;
    }
}
void ConsistencyDriver::presented(const Workspace& model,const WorkbenchUi& ui,const Viewport& viewport) {
    if (done_ || waiting_ || !ready_) return;
    if(environment_ && viewport.error().empty() && (!viewport.current(model.snapshot()->state) || !viewport.camera_current(ui.input().camera()))) return;
    const auto& clouds=model.snapshot()->scene.environment().clouds;
    if(environment_ && clouds.enabled && clouds.coverage>0 && clouds.density>0 && clouds.extinction_per_m>0 &&
        viewport.environment_settings().mode==render::CloudRenderMode::temporal && viewport.environment_warmup()<32 && viewport.error().empty()) return;
    verify(model,ui,viewport);
    const auto& snapshot=*model.snapshot(); const auto& info=viewport.info();
    const auto rgba=viewport.retained_pixels();
    require(rgba.size()==static_cast<std::size_t>(info.width)*info.height*4,"missing published pixels");
    const std::string header="P6\n"+std::to_string(info.width)+" "+std::to_string(info.height)+"\n255\n";
    const auto header_bytes=std::as_bytes(std::span{header.data(),header.size()});
    ByteBuffer ppm{header_bytes.begin(),header_bytes.end()}; ppm.reserve(ppm.size()+rgba.size()/4*3);
    for (std::size_t i=0;i<rgba.size();i+=4) ppm.insert(ppm.end(),rgba.begin()+i,rgba.begin()+i+3);
    const std::string stage=names[stage_];
    check(write_file_bytes_atomic(directory_/(stage+".ppm"),ppm));
    Json entities=Json::array(); for (const auto& e:snapshot.scene.entities()) entities.push_back(entity(e));
    const auto& state=snapshot.state;
    Json report{{"checkpoint",stage},{"state",{{"document_id",state.document_id.to_string()},{"scene_id",state.scene_id.to_string()},
        {"revision",state.revision},{"dirty",state.dirty},{"entity_count",state.entity_count}}},
        {"entities",entities},{"selection",model.selection() ? Json(model.selection()->to_string()) : Json(nullptr)},
        {"inspector",model.draft() ? entity(model.draft()->entity) : Json(nullptr)},{"pending",model.pending()},
        {"history",{{"undo_count",model.history().undo_count},{"redo_count",model.history().redo_count}}},{"ui_errors",ui.errors()},
        {"viewport",{{"document_id",state.document_id.to_string()},{"scene_id",info.scene.to_string()},
            {"revision",info.revision},{"frame",info.frame},{"width",info.width},{"height",info.height},{"draw_count",info.draw_count},
            {"camera",camera(viewport.published_camera())},{"rgba_signature",viewport.pixel_signature()},
            {"image",".dk-consistency/"+stage+".ppm"},{"preview",false}}}};
    if(environment_) {
        report["environment_draft"]=model.environment_draft()->value;
        report["viewport"]["image"]=".dk-environment/"+stage+".ppm";
        report["viewport"]["discarded"]=viewport.environment_discarded();
        const auto& settings=viewport.environment_settings();
        report["viewport"]["render_options"]={{"mode",settings.mode==render::CloudRenderMode::reference ? "reference" : "temporal"},
            {"cloud_shadows",settings.cloud_shadows},{"shadow_center_m",vector(settings.shadow_center_m)},
            {"shadow_extent_m",settings.shadow_extent_m},{"shadow_resolution",settings.shadow_resolution}};
        report["viewport"]["time_seconds"]=settings.time_seconds;
        report["viewport"]["warmup_frames"]=viewport.environment_warmup();
        report["viewport"]["shadow_generation"]=viewport.environment_shadow();
        report["viewport"]["history_reset"]=render::cloud_history_reset_name(viewport.environment_history().reset);
    }
    const auto text=report.dump(2);
    check(write_file_bytes_atomic(directory_/(stage+".json"),std::as_bytes(std::span{text.data(),text.size()})));
    checkpoints_[stage_]=std::move(report); waiting_=true;
    std::printf("Consistency checkpoint %s revision=%llu\n",stage.c_str(),static_cast<unsigned long long>(state.revision));
}
}
