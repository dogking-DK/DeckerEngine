#include "ConsistencyDriver.hpp"
#include "EditorSupport.hpp"
#include <dk/io/File.hpp>
#include <array>
#include <cstdio>

namespace dk::editor::detail {
namespace {
constexpr std::array names={"initial","gui","external","draft","conflict","undo","redo","saved","reloaded"};
void require(bool value,const char* message) {
    if (!value) throw std::runtime_error{std::string{"Consistency smoke: "}+message};
}
std::span<const char* const> actions(std::size_t stage) {
    static constexpr std::array gui={"viewport","name",":all",":name","translation",":all",":translation","apply"};
    static constexpr std::array draft={"name",":all",":draft"};
    static constexpr std::array conflict={"apply","revert"};
    static constexpr std::array undo={"undo"},redo={"redo"},save={"save"},reload={"reload"};
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
    return {{"eye",vector(eye)},{"target",vector(target)},{"up",vector(up)},{"fov_y",65},{"near",0.05},{"far",10000}};
}
}
ConsistencyDriver::ConsistencyDriver(const std::filesystem::path& root) : directory_(root/".dk-consistency") {
    require(std::filesystem::create_directory(directory_),"checkpoint directory must be fresh");
}
void ConsistencyDriver::input(WorkbenchUi& ui) {
    if (done_) return;
    if (waiting_) {
        if (!std::filesystem::is_regular_file(directory_/(std::string{names[stage_]}+".continue"))) return;
        waiting_=false; ready_=false; tick_=0;
        if (++stage_==names.size()) { done_=true; return; }
    }
    auto& io=ImGui::GetIO(); io.ConfigInputTrickleEventQueue=false;
    if (tick_++<12) return;
    const auto sequence=actions(stage_);
    const auto elapsed=tick_-13,step=elapsed/8,phase=elapsed%8;
    if (step>=sequence.size()) { ready_=true; return; }
    const std::string_view action=sequence[step];
    if (phase==0) {
        io.AddFocusEvent(true);
        if (action==":all") { io.AddKeyEvent(ImGuiMod_Ctrl,true); io.AddKeyEvent(ImGuiKey_A,true); }
        else if (action==":name") io.AddInputCharactersUTF8("GUI consistency");
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
        else if (!action.starts_with(":")) io.AddMouseButtonEvent(0,false);
    }
}
void ConsistencyDriver::verify(const Workspace& model,const WorkbenchUi& ui,const Viewport& viewport) const {
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
void ConsistencyDriver::presented(const Workspace& model,const WorkbenchUi& ui,const Viewport& viewport) {
    if (done_ || waiting_ || !ready_) return;
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
    const auto text=report.dump(2);
    check(write_file_bytes_atomic(directory_/(stage+".json"),std::as_bytes(std::span{text.data(),text.size()})));
    checkpoints_[stage_]=std::move(report); waiting_=true;
    std::printf("Consistency checkpoint %s revision=%llu\n",stage.c_str(),static_cast<unsigned long long>(state.revision));
}
}
