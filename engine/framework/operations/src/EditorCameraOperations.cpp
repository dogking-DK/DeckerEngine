#include "EditorHostCommands.hpp"
#include <array>
namespace dk {
namespace {
using namespace editor;
Json number(double low,double high) { auto v=schema::number(); v["minimum"]=low; v["maximum"]=high; return v; }
Json choice(std::initializer_list<const char*> values) { auto s=schema::string(); s["enum"]=values; return s; }
Json vector(double limit=1e9) { return schema::array(number(-limit,limit),3,3); }
Json object_all(Json fields) {
    auto keys=Json::array(); for(const auto& [name,value] : fields.items()) { (void)value; keys.push_back(name); }
    return schema::object(std::move(fields),std::move(keys));
}
Json fields() {
    return {{"navigation_mode",choice({"orbit","fly","walk"})},{"projection",choice({"perspective","orthographic"})},
        {"standard_view",choice({"front","back","left","right","top","bottom"})},
        {"fov_y_deg",number(1,179)},{"ortho_height",number(.01,100000)},{"automatic_clip",schema::boolean()},
        {"near_plane",schema::number()},{"far_plane",schema::number()},
        {"move_speed",number(.01,1000)},{"fast_multiplier",number(1,100)},{"slow_multiplier",number(.01,1)},
        {"look_sensitivity",number(.0001,.05)},{"orbit_sensitivity",number(.0001,.05)},
        {"pan_multiplier",number(.01,10)},{"wheel_sensitivity",number(.01,1)},{"invert_y",schema::boolean()},
        {"eye",vector()},{"yaw_deg",number(-180,180)},{"pitch_deg",number(-90,90)},{"focus_distance",number(.1,3000)}};
}
Json settings_json(const Camera& c) {
    const auto& l=c.settings().lens; const auto& n=c.settings().navigation; const auto p=c.pose();
    constexpr const char* views[]={"none","front","back","left","right","top","bottom"};
    return {{"navigation_mode",c.navigation_mode()==NavigationMode::orbit ? "orbit" : c.navigation_mode()==NavigationMode::fly ? "fly" : "walk"},
        {"projection",c.projection_mode()==ProjectionMode::perspective ? "perspective" : "orthographic"},
        {"standard_view",views[static_cast<unsigned>(c.standard_view())]},
        {"fov_y_deg",l.fov_y},{"ortho_height",l.ortho_height},{"automatic_clip",l.automatic_clip},{"near_plane",l.near_plane},{"far_plane",l.far_plane},
        {"move_speed",n.move_speed},{"fast_multiplier",n.fast_multiplier},{"slow_multiplier",n.slow_multiplier},
        {"look_sensitivity",n.look_sensitivity},{"orbit_sensitivity",n.orbit_sensitivity},{"pan_multiplier",n.pan_multiplier},
        {"wheel_sensitivity",n.wheel_sensitivity},{"invert_y",n.invert_y},
        {"eye",{p.eye.x(),p.eye.y(),p.eye.z()}},{"yaw_deg",p.yaw_deg},{"pitch_deg",p.pitch_deg},{"focus_distance",p.focus_distance}};
}
Json state_json(const EditorCameraState& state) {
    auto base=editor_viewport_state_json(state.viewport);
    return {{"guard",base["guard"]},{"settings",settings_json(state.camera)},{"render_revision",state.camera.revision()},
        {"effective_clip",{{"near_plane",state.camera.near_plane()},{"far_plane",state.camera.far_plane()}}},
        {"aspect",state.aspect},{"blocked_reason",base["blocked_reason"]},{"presentation",base["presentation"]}};
}
template<class T> void assign(const Json& p,const char* name,T& target) { if(p.contains(name)) target=p.at(name).get<T>(); }
Vec3d vec(const Json& p) { return {p[0].get<double>(),p[1].get<double>(),p[2].get<double>()}; }
Result<EditorViewportGuard> read_guard(const Json& v) {
    auto view=EditorViewId::parse(v.at("view_id").get<std::string>()); if(!view) return std::unexpected(view.error());
    auto doc=DocumentId::parse(v.at("document_id").get<std::string>()); if(!doc) return std::unexpected(doc.error());
    return EditorViewportGuard{*view,*doc,v.at("revision").get<std::uint64_t>()};
}
Result<void> patch(Camera& c,const Json& p,double aspect) {
    const bool pose=p.contains("eye") || p.contains("yaw_deg") || p.contains("pitch_deg") || p.contains("focus_distance");
    if(pose && p.contains("standard_view")) return std::unexpected(Error{ErrorCode::invalid_argument,"Pose and standard_view are mutually exclusive"});
    if(p.contains("projection")) if(auto r=c.set_projection_mode(p["projection"]=="perspective" ? ProjectionMode::perspective : ProjectionMode::orthographic,aspect); !r) return r;
    auto settings=c.settings(); auto& l=settings.lens; auto& n=settings.navigation;
    assign(p,"fov_y_deg",l.fov_y); assign(p,"ortho_height",l.ortho_height); assign(p,"automatic_clip",l.automatic_clip);
    assign(p,"near_plane",l.near_plane); assign(p,"far_plane",l.far_plane); assign(p,"move_speed",n.move_speed);
    assign(p,"fast_multiplier",n.fast_multiplier); assign(p,"slow_multiplier",n.slow_multiplier); assign(p,"look_sensitivity",n.look_sensitivity);
    assign(p,"orbit_sensitivity",n.orbit_sensitivity); assign(p,"pan_multiplier",n.pan_multiplier); assign(p,"wheel_sensitivity",n.wheel_sensitivity); assign(p,"invert_y",n.invert_y);
    if(auto r=c.set_settings(settings,aspect); !r) return r;
    if(p.contains("navigation_mode")) if(auto r=c.set_navigation_mode(p["navigation_mode"]=="orbit" ? NavigationMode::orbit : p["navigation_mode"]=="fly" ? NavigationMode::fly : NavigationMode::walk); !r) return r;
    if(pose) {
        auto value=c.pose(); if(p.contains("eye")) value.eye=vec(p["eye"]);
        assign(p,"yaw_deg",value.yaw_deg); assign(p,"pitch_deg",value.pitch_deg); assign(p,"focus_distance",value.focus_distance);
        if(auto r=c.set_pose(value,aspect); !r) return r;
    }
    if(p.contains("standard_view")) {
        constexpr const char* names[]={"front","back","left","right","top","bottom"};
        for(unsigned i=0;i<6;++i) if(p["standard_view"]==names[i]) return c.set_standard_view(static_cast<StandardView>(i+1),aspect);
    }
    return {};
}
}
std::vector<EditorCommandEntry> editor_camera_commands(EditorHost host,const Json& viewport_result) {
    const auto& vp=viewport_result.at("properties");
    auto output_fields=fields(); output_fields["standard_view"]["enum"].push_back("none");
    // GUI gestures can exceed command pose limits; output describes all existing valid camera values.
    output_fields["eye"]=schema::array(schema::number(),3,3);
    output_fields["focus_distance"]=schema::number();
    const auto result=object_all({{"guard",vp["guard"]},{"settings",object_all(std::move(output_fields))},
        {"render_revision",vp["guard"]["properties"]["revision"]},{"aspect",schema::number()},
        {"effective_clip",object_all({{"near_plane",schema::number()},{"far_plane",schema::number()}})},
        {"blocked_reason",vp["blocked_reason"]},{"presentation",vp["presentation"]}});
    std::vector<EditorCommandEntry> entries;
    for(const std::string action : {"query","set","orbit","pan","zoom","frame","reset"}) {
        auto properties=Json::object(); auto required=Json::array(); auto help=Json::object();
        if(action=="set") properties=fields();
        if(action=="orbit") properties={{"yaw_deg",number(-360,360)},{"pitch_deg",number(-180,180)}};
        if(action=="pan") properties={{"right",number(-1e6,1e6)},{"up",number(-1e6,1e6)}};
        if(action=="zoom") { properties["steps"]=number(-1000,1000); required.push_back("steps"); }
        if(action=="frame") properties={{"entity",schema::string(36,36)},
            {"bounds",object_all({{"minimum",vector()},{"maximum",vector()}})}};
        if(action=="reset") properties["defaults"]=schema::boolean();
        if(action!="query") { properties["guard"]=vp["guard"]; required.push_back("guard"); }
        for(const auto& [name,value] : properties.items()) { (void)value;
            help["/properties/"+name]={{"description","Edit camera session value; omitted set fields preserve current values. Does not edit scene/history."}};
            if(name.ends_with("_deg")) help["/properties/"+name]["unit"]="deg";
            if(name=="right" || name=="up" || name=="eye" || name=="near_plane" || name=="far_plane" || name=="ortho_height" || name=="focus_distance") help["/properties/"+name]["unit"]="m";
            if(name=="move_speed") help["/properties/"+name]["unit"]="m/s";
            if(name=="look_sensitivity" || name=="orbit_sensitivity") help["/properties/"+name]["unit"]="rad/pixel";
        }
        if(action!="query") help["/properties/guard"]["description"]="Latest Edit view/document/revision guard shared with viewport commands; GUI camera changes invalidate old guards.";
        if(action=="query") help[""]={{"description","Read the Edit camera, shared guard and completed image provenance; Simulation camera is independent."}};
        if(action=="set") {
            const std::pair<const char*,const char*> descriptions[]={
                {"navigation_mode","Orbit, Fly or fixed-height Walk without collision/gravity. Orthographic supports Orbit only; mode does not change the image."},
                {"projection","Convert perspective/orthographic at the focus plane; returning to perspective restores its previous navigation mode."},
                {"standard_view","Orthographic view about the current pivot; mutually exclusive with explicit pose fields."},
                {"fov_y_deg","Perspective vertical FOV; strictly between 1 and 179 degrees and within current backend support."},
                {"ortho_height","Full orthographic visible height; applied after projection conversion."},
                {"automatic_clip","Derive clipping from current renderer and scene environment; retain manual near/far values."},
                {"near_plane","Manual near clipping distance; requires 0 < near < far, including when retained under automatic clipping."},
                {"far_plane","Manual far clipping distance; must exceed near and satisfy the active backend support domain."},
                {"move_speed","Base Fly/Walk movement speed; does not reset path samples."},
                {"fast_multiplier","Shift movement multiplier; Ctrl takes precedence when both are held."},
                {"slow_multiplier","Ctrl movement multiplier; does not change the rendered view."},
                {"look_sensitivity","Fly/Walk mouse look radians per logical pixel."},
                {"orbit_sensitivity","Mouse Orbit radians per logical pixel; angle-based orbit commands are independent."},
                {"pan_multiplier","Mouse pan scale; meter-based pan commands are independent."},
                {"wheel_sensitivity","Exponential zoom coefficient per wheel step; shared by zoom commands."},
                {"invert_y","Invert mouse vertical look/orbit; command angle and translation signs are unchanged."},
                {"eye","World-space eye position; preserves omitted orientation and focus distance."},
                {"yaw_deg","Absolute azimuth in degrees, with no roll; cannot be combined with standard_view."},
                {"pitch_deg","Absolute elevation in degrees, with no roll; cannot be combined with standard_view."},
                {"focus_distance","Distance from eye to pivot along camera forward; changing only this does not move the eye."}};
            for(const auto& [name,description] : descriptions) help[std::string("/properties/")+name]["description"]=description;
        }
        if(action=="orbit") { help["/properties/yaw_deg"]["description"]="Orbit yaw increment about the pivot; positive increases azimuth, independent of mouse sensitivity."; help["/properties/pitch_deg"]["description"]="Orbit pitch increment; positive raises the eye, final pitch saturates at +/-1.55 rad."; }
        if(action=="pan") { help["/properties/right"]["description"]="Translation along camera right in meters."; help["/properties/up"]["description"]="Translation along camera up in meters."; }
        if(action=="zoom") help["/properties/steps"]={{"unit","wheel steps"},{"description","Positive zooms in using the current wheel sensitivity; distance/orthographic height saturate at supported limits."}};
        if(action=="frame") help["/properties/entity"]={{"completion_provider","scene.entity"},{"description","Frame this entity subtree using current published Edit geometry; mutually exclusive with explicit world bounds."}};
        if(action=="reset") help["/properties/defaults"]={{"default",false},{"description","False restores pose only; true also restores navigation mode, projection and all lens/navigation settings."}};
        entries.push_back({{"editor.camera."+action,"Edit camera: "+action,schema::object(std::move(properties),std::move(required)),result,
            action=="query" ? CommandEffect::query : CommandEffect::control,false,Json{{"version",1},{"fields",std::move(help)}}},
            [host,action](const Json& p)->Result<Json> {
                auto service=host(); if(!service) return std::unexpected(Error{ErrorCode::invalid_state,"Editor camera host is detached"});
                auto state=service->query_camera(); if(!state) return std::unexpected(state.error());
                if(action=="query") return state_json(*state);
                auto guard=read_guard(p.at("guard")); if(!guard) return std::unexpected(guard.error());
                if(*guard!=state->viewport.guard) return std::unexpected(Error{ErrorCode::conflict,"Camera guard is stale"});
                if(!state->viewport.blocked_reason.empty()) return std::unexpected(Error{ErrorCode::invalid_state,state->viewport.blocked_reason});
                auto candidate=state->camera; Result<void> change;
                if(action=="set" || action=="orbit" || action=="pan") {
                    if(p.size()==1) return std::unexpected(Error{ErrorCode::invalid_argument,"At least one setting or delta is required"});
                }
                if((action=="orbit" || action=="pan" || action=="zoom") && candidate.navigation_mode()!=NavigationMode::orbit)
                    return std::unexpected(Error{ErrorCode::invalid_state,"Orbit/pan/zoom commands require Orbit navigation"});
                if(action=="set") change=patch(candidate,p,state->aspect);
                else if(action=="orbit") change=candidate.orbit_degrees(p.value("yaw_deg",0.),p.value("pitch_deg",0.),state->aspect);
                else if(action=="pan") change=candidate.pan_meters(p.value("right",0.),p.value("up",0.),state->aspect);
                else if(action=="zoom") candidate.dolly(p.at("steps").get<double>());
                else if(action=="frame") {
                    if(p.contains("entity")==p.contains("bounds")) return std::unexpected(Error{ErrorCode::invalid_argument,"Frame requires exactly one of entity or bounds"});
                    if(p.contains("entity")) {
                        auto id=EntityId::parse(p.at("entity").get<std::string>()); if(!id) return std::unexpected(id.error());
                        auto framed=service->frame_camera(*guard,*id); if(!framed) return std::unexpected(framed.error()); return state_json(*framed);
                    }
                    change=candidate.frame({vec(p["bounds"]["minimum"]),vec(p["bounds"]["maximum"])},state->aspect);
                } else if(action=="reset") {
                    auto reset=service->reset_camera(*guard,p.value("defaults",false)); if(!reset) return std::unexpected(reset.error()); return state_json(*reset);
                }
                if(!change) return std::unexpected(change.error());
                auto committed=service->commit_camera(*guard,candidate); if(!committed) return std::unexpected(committed.error()); return state_json(*committed);
            }});
    }
    return entries;
}
}
