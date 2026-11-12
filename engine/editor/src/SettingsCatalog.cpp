#include "SettingsCatalog.hpp"
#include "CommandFieldHelp.hpp"
#include "ConsoleHistory.hpp"
#include <algorithm>
#include <array>
#include <map>
#include <sstream>

namespace dk::editor::detail {
namespace {
std::string lower(std::string value) { for(auto& c:value) if(c>='A' && c<='Z') c+=('a'-'A'); return value; }
std::string pointer(std::string_view parameter) {
    std::string p; std::istringstream input{std::string(parameter)}; std::string key;
    while(std::getline(input,key,'.')) p+="/properties/"+key;
    return p;
}
// Deliberately invalid typed placeholders: neither defaults nor live guards are inserted.
Json placeholder(const Json& schema,const std::string& name,const std::vector<std::string>& route={},std::size_t depth=0) {
    if(depth>16) return "<"+name+">";
    if(schema.value("type",std::string{})=="object") {
        Json out=Json::object();
        for(const auto& key:schema.value("required",Json::array())) {
            const auto k=key.get<std::string>(); out[k]=placeholder(schema["properties"][k],k,{},depth+1);
        }
        if(!route.empty() && schema.contains("properties") && schema["properties"].contains(route.front())) {
            out[route.front()]=placeholder(schema["properties"][route.front()],route.front(),{route.begin()+1,route.end()},depth+1);
        }
        return out;
    }
    if(schema.value("type",std::string{})=="array") {
        Json out=Json::array(); const auto count=std::min(schema.value("minItems",1u),16u);
        for(unsigned i=0;i<count;++i) out.push_back(placeholder(schema.at("items"),name+"_"+std::to_string(i),{},depth+1));
        return out;
    }
    return "<"+name+">";
}
std::string make_template(const CommandDescriptor& command,const std::string& path) {
    std::vector<std::string> route; std::istringstream parts{path}; std::string part;
    while(std::getline(parts,part,'/')) if(!part.empty() && part!="properties") route.push_back(part);
    auto values=placeholder(command.parameters,"params",route);
    std::string text=command.name;
    for(const auto& [key,value]:values.items()) text+=" "+key+"="+value.dump();
    return text.size()<=console_input_limit ? text : std::string{};
}
}
std::vector<SettingInfo> build_settings_catalog(const CommandCatalogSnapshot& catalog) {
    std::vector<SettingInfo> result;
    const std::string scene="Draft until Apply; committed changes update Scene/Undo and are saved with the scene. Render history follows scene changes.";
    const std::string view="Edit view session only; no Scene/Undo/file change. Physical inputs reset sampling; exposure/target retain samples.";
    const std::string camera="Edit camera session only; no Scene/Undo/file change. Pose/lens affect rendering; navigation preferences preserve samples.";
    const std::string simulation="Next-experiment draft only; used on explicit Play/Prepare. Does not change the active run or Scene/Undo.";
    auto add=[&](std::string id,std::string panel,std::string section,std::string label,std::string aliases,
        std::string command,std::string parameter,std::string explanation,std::string impact) {
        SettingInfo s{std::move(id),std::move(panel),std::move(section),std::move(label),std::move(aliases),std::move(command),pointer(parameter),std::move(explanation),std::move(impact)};
        if(s.id.starts_with("camera.")) s.aliases+=" 相机";
        if(s.panel=="Path tracing") s.aliases+=" 路径追踪 光追";
        const auto it=std::ranges::find(catalog.commands,s.command,&CommandDescriptor::name);
        if(!s.command.empty() && it!=catalog.commands.end()) {
            const Json::json_pointer p(s.path);
            if(it->parameters.contains(p)) {
                s.command_available=true;
                const auto& schema=it->parameters.at(p);
                bool required=false;
                if(!s.path.empty()) { auto parent=p; const auto name=parent.back(); parent.pop_back(); parent.pop_back();
                    const auto& object=it->parameters.at(parent);
                    const auto keys=object.value("required",Json::array()); required=std::ranges::find(keys,Json(name))!=keys.end(); }
                s.help=command_field_help(&*it,s.path,schema,required);
                s.command_template=make_template(*it,s.path);
                if(it->help && it->help->at("fields").contains(s.path)) {
                    const auto& f=it->help->at("fields")[s.path];
                    if(f.contains("aliases")) s.aliases+=" "+f["aliases"].dump();
                    if(f.contains("label")) s.aliases+=" "+f["label"].get<std::string>();
                }
            }
        }
        s.search_text=lower(s.id+" "+s.panel+" "+s.section+" "+s.label+" "+s.aliases+" "+s.command+" "+s.path+" "+s.help);
        result.push_back(std::move(s));
    };
    add("console.input","Console","Input","Command input","命令 输入 控制台","scene.query","limit","Named parameters: scene.query limit=20. Tab: candidates; Enter accepts, then a second Enter executes. JSON is also accepted; explicit guards required.","Typing/assistance has no task or scene effect; Run dispatches the command. History is session-only.");
    add("console.run","Console","Input","Run command","执行 命令 控制台","","","Runs the current Console text through the command registry. Required parameters, guards and service conditions are checked at execution.","Effects and Undo depend on the command. No automatic guard.");
    for(const auto& [key,label,aliases,info]:std::initializer_list<std::array<const char*,4>>{
        {"name","Entity name","名称 实体","Entity display name; editing the field creates an Inspector draft."},
        {"translation","Translation","位置 平移 变换","Local XYZ translation in meters."},
        {"rotation","Quaternion","旋转 四元数 变换","Local quaternion in XYZW order; finite, nonzero, normalized on commit."},
        {"scale","Scale","缩放 变换","Local XYZ scale; finite, including zero or negative values."}})
        add("inspector."+std::string(key),"Inspector","Local transform",label,aliases,key==std::string_view("name") ? "entity.set_name" : "entity.set_transform",key==std::string_view("name") ? "name" : "transform."+std::string(key),info,scene);
    add("inspector.apply","Inspector","Draft","Apply Inspector","应用 变换","","","Commit the current Inspector draft through the guarded scene edit path.",scene);
    add("inspector.revert","Inspector","Draft","Revert Inspector","撤回 草稿","","","Discard the current Inspector draft and restore committed values.","Does not create Undo or modify the Scene.");
    add("environment.enabled","Environment","General","Enable atmosphere","大气 启用","environment.set","environment.enabled","Enable the scene atmosphere; cloud enable is separate.",scene);
    add("environment.preset","Environment","General","Environment preset","预设 晴天 阴天 日落","environment.preset","name","GUI loads a preset into the draft; Apply is still required. The related query command returns preset values only.",scene);
    const auto env=[&](const char* group,const char* key,const char* label,const char* alias,const char* info) {
        // Additional physical constraints live in Scene validation, beyond the JSON shape schema.
        static const std::map<std::string,std::string> bounds={
            {"planet_center_m","Each component [-1e9,1e9]."},{"ground_radius_m","[1,1e9]."},{"atmosphere_height_m","[1,1e7]."},
            {"rayleigh_scattering_per_m","Each channel [0,1]."},{"mie_scattering_per_m","Each channel [0,1]."},
            {"mie_extinction_per_m","Each channel [0,1]."},{"absorption_extinction_per_m","Each channel [0,1]."},{"ground_albedo","Each channel [0,1]."},
            {"rayleigh_scale_height_m","[1,atmosphere_height_m]."},{"mie_scale_height_m","[1,atmosphere_height_m]."},
            {"mie_anisotropy","[-0.99,0.99]."},{"absorption_center_height_m","[0,atmosphere_height_m]."},{"absorption_half_width_m","[1,atmosphere_height_m]."},
            {"irradiance","Each channel [0,1e6]."},{"angular_radius_rad","[1e-6,0.1]."},
            {"weather_resolution","Power of two, [8,256]."},{"base_resolution","Power of two, [8,128]."},{"detail_resolution","Power of two, [4,64]."},
            {"bottom_height_m","0 <= bottom < top < atmosphere_height_m."},{"top_height_m","0 <= bottom < top < atmosphere_height_m."},
            {"coverage","[0,1]."},{"density","[0,10]."},{"extinction_per_m","[0,1]."},
            {"weather_scale_m","[1,1e7]."},{"shape_scale_m","[1,1e7]."},{"detail_scale_m","[1,1e7]."},
            {"bottom_fade_fraction","[0.001,0.5]."},{"top_fade_fraction","[0.001,0.5]."},{"wind_velocity_mps","Each component [-1000,1000]."}};
        std::string explanation=info;
        if(const auto it=bounds.find(key);it!=bounds.end()) explanation+=" Apply validation: "+it->second;
        add("environment."+std::string(group)+"."+key,"Environment",group==std::string_view("sun") ? "Sun" : group==std::string_view("atmosphere") ? "Atmosphere parameters" : "Cloud physical parameters",label,alias,"environment.set","environment."+std::string(group)+"."+key,explanation,scene);
    };
    env("sun","direction","Sun direction","太阳 方向 光照","World direction toward the sun; finite nonzero XYZ, normalized on commit.");
    env("sun","irradiance","Sun irradiance","太阳 强度 光照","Scene-linear RGB normal irradiance, not lux.");
    env("sun","angular_radius_rad","Sun angular radius","太阳 角半径 光照","Angular radius in radians.");
    for(const auto& [key,alias,info]:std::initializer_list<std::array<const char*,3>>{
        {"planet_center_m","大气 星球 中心","World XYZ planet center in meters; atmosphere height is measured from its sphere."},
        {"ground_radius_m","大气 星球 半径","Ground sphere radius in meters, independent of the scene triangle mesh."},
        {"atmosphere_height_m","大气 厚度 高度","Distance from ground to atmosphere top in meters."},
        {"rayleigh_scattering_per_m","大气 瑞利 散射","Sea-level linear RGB Rayleigh scattering in inverse meters; also Rayleigh extinction."},
        {"rayleigh_scale_height_m","大气 瑞利 标高","Rayleigh density exp(-height/scale_height), in meters; cannot exceed atmosphere thickness."},
        {"mie_scattering_per_m","大气 米氏 散射","Sea-level linear RGB Mie scattering in inverse meters; each channel must not exceed Mie extinction."},
        {"mie_extinction_per_m","大气 米氏 消光","Sea-level linear RGB Mie extinction in inverse meters, including scattering and absorption."},
        {"mie_scale_height_m","大气 米氏 标高","Mie density exponential scale height in meters; cannot exceed atmosphere thickness."},
        {"mie_anisotropy","大气 米氏 各向异性","Dimensionless Mie phase asymmetry; positive values favor forward scattering."},
        {"absorption_extinction_per_m","大气 吸收 消光","Linear RGB absorption coefficient in inverse meters, multiplied by the triangular absorption layer."},
        {"absorption_center_height_m","大气 吸收 中心 高度","Peak of the triangular absorption layer, meters above ground; within atmosphere thickness."},
        {"absorption_half_width_m","大气 吸收 半宽","Distance from absorption peak to zero density in meters; cannot exceed atmosphere thickness."},
        {"ground_albedo","大气 地面 反照率","Dimensionless linear RGB ground reflectance used by atmosphere lighting."}})
        env("atmosphere",key,key,alias,info);
    for(const auto& [key,alias,info]:std::initializer_list<std::array<const char*,3>>{
        {"enabled","云 启用","Enable the physical cloud layer independently from atmosphere enable."},
        {"seed","云 种子","Unsigned deterministic generator seed; changing it rebuilds cloud textures."},
        {"weather_resolution","云 天气 分辨率","2D weather texture edge length in texels; power of two required."},
        {"base_resolution","云 基础 分辨率","3D base-noise texture edge length in texels; power of two required."},
        {"detail_resolution","云 细节 分辨率","3D detail-noise texture edge length in texels; power of two required."},
        {"bottom_height_m","云 底部 高度","Cloud bottom spherical altitude in meters; below cloud top."},
        {"top_height_m","云 顶部 高度","Cloud top spherical altitude in meters; above bottom and below atmosphere top."},
        {"coverage","云 覆盖率","Dimensionless cloud coverage; zero gives exactly no clouds. Does not rebuild textures."},
        {"density","云 密度","Dimensionless density multiplier; zero gives exactly no clouds. Does not rebuild textures."},
        {"extinction_per_m","云 消光","Cloud extinction coefficient in inverse meters; zero gives exactly no clouds."},
        {"weather_scale_m","云 天气 尺度","World weather texture period in meters."},
        {"shape_scale_m","云 形状 尺度","World base-shape texture period in meters."},
        {"detail_scale_m","云 细节 尺度","World detail texture period in meters."},
        {"bottom_fade_fraction","云 底部 渐变","Bottom density fade thickness as a fraction of cloud shell thickness."},
        {"top_fade_fraction","云 顶部 渐变","Top density fade thickness as a fraction of cloud shell thickness."},
        {"wind_velocity_mps","云 风 速度","World X/Z cloud translation velocity in meters per second; time is a separate render input."}})
        env("clouds",key,key,alias,info);
    add("environment.apply","Environment","Draft","Apply environment","应用 环境","environment.set","environment","Replace the complete scene environment using the guarded draft.",scene);
    add("environment.revert","Environment","Draft","Revert environment","撤回 环境 草稿","","","Restore the committed scene environment without changing the document.","Discards the environment draft only; no Undo entry.");
    add("viewport.raster_exposure","Environment","Viewport","Raster exposure","光栅 曝光","editor.viewport.set","raster_exposure","Linear raster exposure multiplier; independent from path tracing EV.",view);
    add("viewport.renderer","Viewport","View","Renderer","渲染器 光线追踪 光栅","editor.viewport.set","renderer","Raster or Path trace. Path trace requires the module and compatible GPU.",view);
    add("viewport.unlit_inspection","Viewport","View","Unlit inspection","无光照 检查","editor.viewport.set","unlit_inspection","Inspect raster geometry without scene atmosphere/light.",view);
    add("viewport.voxel_shading","Viewport","Voxel","Voxel shading","体素 光照","editor.viewport.set","voxel_shading","Unlit or nonmetallic directional solid voxel shading.",view);
    add("viewport.voxel_light_direction","Viewport","Voxel","Voxel light direction","体素 光源 方向","editor.viewport.set","voxel_light_direction","Unit world direction towards the light.",view);
    add("viewport.voxel_irradiance","Viewport","Voxel","Voxel irradiance","体素 辐照度","editor.viewport.set","voxel_irradiance","Linear RGB irradiance for directional voxel shading.",view);
    for(const auto& [key,label,info]:std::initializer_list<std::array<const char*,3>>{
        {"mode","Cloud render mode","Temporal progressively refines while stationary; Reference requires at most 4096 viewport pixels."},
        {"time_seconds","Cloud render time","Fixed time in seconds; changes reset cloud history. Initial UI value 0."},
        {"cloud_shadows","Cloud shadows","Toggle raster cloud shadows; session-only, not the physical cloud enable."},
        {"shadow_center_m","Shadow center","World XYZ center in meters; each component must be finite, absolute value <= 1e9."},
        {"shadow_extent_m","Shadow extent","Floating-point world extent in meters, 16..65536; validated by the renderer."},
        {"shadow_resolution","Shadow resolution","Integer shadow-map edge in pixels, 16..512 and a power of two; validated by the renderer."}})
        add("preview."+std::string(key),"Environment","Cloud render quality and time",label,"预览 云 阴影 时间 质量","","",info,"Local raster preview preference; no Scene/Undo/file change. No command interface. Changes may reset cloud history.");
    for(const auto& [key,label,alias]:std::initializer_list<std::array<const char*,3>>{
        {"navigation_mode","Navigation mode","导航 漫游 飞行 环绕"},{"projection","Projection","投影 透视 正交"},{"standard_view","Standard view","标准视图 顶视图 六视图"},
        {"fov_y_deg","Vertical FOV","视场角 视野 镜头"},{"ortho_height","Orthographic height","正交 高度 镜头"},{"automatic_clip","Automatic clipping","自动 裁剪"},
        {"near_plane","Near plane","近裁剪 镜头"},{"far_plane","Far plane","远裁剪 镜头"},
        {"orbit_sensitivity","Orbit sensitivity","环绕 灵敏度"},{"pan_multiplier","Pan multiplier","平移 灵敏度"},{"wheel_sensitivity","Wheel sensitivity","滚轮 缩放 灵敏度"},
        {"invert_y","Invert mouse Y","反转 鼠标"},{"move_speed","Move speed","移速 速度 漫游"},{"fast_multiplier","Fast multiplier","加速 倍率"},
        {"slow_multiplier","Slow multiplier","减速 倍率"},{"look_sensitivity","Look sensitivity","观察 灵敏度"}}) {
        const bool mode=key==std::string_view("navigation_mode") || key==std::string_view("projection") || key==std::string_view("standard_view");
        const bool prefs=key==std::string_view("fast_multiplier") || key==std::string_view("slow_multiplier") || key==std::string_view("look_sensitivity");
        add("camera."+std::string(key),"Viewport",mode ? "View" : prefs ? "Camera/Preferences" : "Camera/Lens and navigation",label,alias,"editor.camera.set",key,"Camera popup changes remain a draft until Apply. Mode/projection buttons take effect immediately.",camera);
    }
    for(const auto* action:{"frame","reset"}) add("camera."+std::string(action),"Viewport","View",action,"相机 聚焦 重置","editor.camera."+std::string(action),action==std::string_view("frame") ? "entity" : "defaults",action==std::string_view("frame") ? "Frame the selected entity and subtree using current published Edit geometry." : "Reset pose; defaults=true also restores lens/navigation preferences.",camera);
    add("camera.apply","Viewport","Camera/Lens and navigation","Apply camera","应用 相机","","","Validate and commit the complete camera draft; failures retain the draft and current camera.",camera);
    add("camera.cancel","Viewport","Camera/Lens and navigation","Cancel camera","取消 相机","","","Close the popup and discard its unapplied values.","No camera/Scene/Undo change.");
    add("camera.defaults","Viewport","Camera/Lens and navigation","Restore camera defaults","恢复 默认 相机","editor.camera.reset","defaults","Restore pose, projection, navigation mode and all preferences; use defaults=true in the command.",camera);
    for(const auto& [key,label,alias]:std::initializer_list<std::array<const char*,3>>{
        {"medium_mode","Medium mode","介质 表面 大气 云"},{"direct_lighting","Direct lighting","直接光照 ReSTIR 算法"},{"initial_candidates","Initial candidates","候选数量 ReSTIR"},
        {"temporal_reuse","Temporal reuse","时间复用 ReSTIR"},{"spatial_reuse","Spatial reuse","空间复用 ReSTIR"},{"target_spp","Target spp","采样 目标 次数"},
        {"seed","Seed","随机 种子"},{"bounces","Bounces","反弹 次数"},{"environment_rgb","Environment RGB","环境 颜色 光照"},{"sky_enabled","Sky","天空 开关"},
        {"sun","Scene sun","太阳 开关 光照"},{"exposure_ev","Exposure EV","曝光"},{"time_seconds","Medium time","介质 时间"},{"cloud_albedo","Cloud albedo","云 反照率"},{"cloud_anisotropy","Cloud anisotropy","云 各向异性"}})
        add("path."+std::string(key),"Path tracing","Settings",label,alias,"editor.viewport.set",key,"Edit path tracing preference. Disabled dependent controls retain their values until their mode is enabled.",view);
    add("path.pause","Path tracing","Controls","Pause / Resume","暂停 继续","editor.viewport.pause","","Pause further sampling; the GUI button resumes when already paused. In-flight work may complete.","Preserves completed samples and old image. Session-only; no Undo.");
    add("path.reset","Path tracing","Controls","Reset accumulation","重置 清空 累积","editor.viewport.reset","","Clear path accumulation; the last complete image remains until replacement.",view);
    const auto sim=[&](const char* key,const char* parameter,const char* label,const char* alias,const char* info) {
        add("simulation."+std::string(key),"Simulation","Next experiment",label,alias,"simulation.run",parameter,info,simulation);
    };
    sim("solver","solver","Solver","求解器 模拟","CPU XPBD or compiled GPU XPBD; an active run is unchanged.");
    sim("count","count","Target steps","步数 目标 模拟","Absolute finite experiment step count.");
    sim("batch_steps","batch_steps","Batch steps","批次 步数 模拟","Steps per submitted batch; larger batches may increase control latency.");
    sim("fixed_dt_ns","fixed_dt_ns","Fixed dt","时间步长 模拟","Fixed timestep in nanoseconds; independent of wall-clock frame duration.");
    for(const auto& [key,label,info]:std::initializer_list<std::array<const char*,3>>{
        {"columns","Columns","Number of cloth columns."},{"rows","Rows","Number of cloth rows."},{"seed","Seed","Deterministic initial perturbation seed."},
        {"spacing","Spacing","Particle spacing in meters."},{"height","Height","Initial cloth height in meters."},{"particle_mass","Mass","Mass per particle in kilograms."},
        {"compliance","Compliance","XPBD distance compliance in m/N; zero is rigid."},{"iterations","Iterations","Constraint iterations per step."},
        {"gravity_y","Gravity","World Y acceleration in m/s2."},{"floor_y","Floor","Collision floor Y in meters."},{"damping","Damping","Velocity damping coefficient in 1/s."}})
        sim(key,("cloth."+std::string(key)).c_str(),label,key==std::string_view("gravity_y") ? "模拟 重力 加速度" : key==std::string_view("particle_mass") ? "模拟 质量" : key==std::string_view("damping") ? "模拟 阻尼" : key==std::string_view("floor_y") ? "模拟 地板" : "布料 物理 模拟 参数",info);
    add("simulation.reset_draft","Simulation","Next experiment","Reset experiment draft","重置 模拟 草稿","","","Restore the next-experiment draft to its UI defaults; no change to the active run.",simulation);
    for(const auto* action:{"play","prepare","pause","resume","step","cancel","stop"})
        add("simulation."+std::string(action),"Simulation","Controls",action,"模拟 开始 暂停 继续 单步 取消 停止",action==std::string_view("play") || action==std::string_view("prepare") ? "simulation.run" : "simulation."+std::string(action),action==std::string_view("prepare") ? "paused" : "","Controls the identified independent experiment; use the command schema for required identity and state.","Runtime control, not Scene/Undo. Play/Prepare use the next-experiment draft; template contains placeholders only.");
    for(const auto* key:{"output","width","height"})
        add("export."+std::string(key),"Simulation","Results/Export options",key,"导出 输出 图片 尺寸","simulation.export",key,"Export options; output is a new project-relative directory, dimensions are pixels. Blank GUI output chooses a run/step directory.","Options are UI-only until explicit Export; Export reads back GPU data and writes a new directory, not Undo.");
    for(const auto* action:{"sample","export"}) add("simulation."+std::string(action),"Simulation","Results",action,"模拟 采样 导出","simulation."+std::string(action),"","Requires a healthy paused completed run. Sampling/export can wait for GPU readback.",action==std::string_view("export") ? "Writes new experiment files; not Undo." : "Updates the diagnostic sample; no Scene edit.");
    std::ranges::sort(result,{},&SettingInfo::id); return result;
}
SettingSearch search_settings(const std::vector<SettingInfo>& entries,std::string_view query) {
    SettingSearch found;
    if(query.size()>256 || console_text_problem(query)) { found.error="Search requires at most 256 UTF-8 bytes on one line."; return found; }
    std::vector<std::string> tokens; std::istringstream input{lower(std::string(query))}; std::string token;
    while(input>>token) tokens.push_back(token);
    for(std::size_t i=0;i<entries.size();++i) if(std::ranges::all_of(tokens,[&](const auto& t){return entries[i].search_text.find(t)!=std::string::npos;})) {
        if(found.indices.size()==64) { found.truncated=true; break; } found.indices.push_back(i);
    }
    return found;
}
std::string setting_unavailable(const SettingInfo& s,const SettingsContext& c) {
    const auto& id=s.id;
    if(!s.command.empty() && !s.command_available) return "The related command is unavailable in this host/build.";
    if(id.starts_with("console.")) return c.composing ? "Finish IME composition first." : "";
    if(id.starts_with("inspector.")) {
        if(!c.selection) return "Select an entity in Hierarchy first.";
        if(c.gizmo) return "Finish the active Gizmo first.";
        if(c.environment_draft) return "Apply or Revert the Environment draft first.";
        if((id=="inspector.apply" || id=="inspector.revert") && !c.inspector_draft) return "No modified Inspector draft.";
    } else if(id.starts_with("environment.")) {
        if(!c.scene) return "Open a scene first.";
        if(c.gizmo) return "Finish the active Gizmo first.";
        if(c.inspector_draft) return "Apply or Revert the Inspector draft first.";
        if((id=="environment.apply" || id=="environment.revert") && !c.environment_draft) return "No modified Environment draft.";
    } else if(id.starts_with("preview.")) {
        if(!c.scene) return "Open a scene first.";
    } else if(id.starts_with("camera.")) {
        if(!c.scene) return "Open a scene first.";
        if(c.simulation_view) return "Switch to Edit view to locate the Edit camera; Simulation camera is independent.";
        if(c.gizmo) return "Finish the active Gizmo first.";
        if(id=="camera.frame" && !c.selection) return "Select an entity to Frame first.";
        if(id=="camera.fov_y_deg" && c.orthographic) return "Perspective FOV is retained while Orthographic is active.";
        if((id=="camera.ortho_height" || id=="camera.standard_view") && !c.orthographic) return "Select Orthographic to use this control.";
        if((id=="camera.near_plane" || id=="camera.far_plane") && c.automatic_clip) return "Turn off Automatic clipping to edit manual planes.";
    } else if(id.starts_with("path.") || id.starts_with("viewport.")) {
        if(!c.scene) return "Open a scene first.";
        if(id=="viewport.renderer" && c.simulation_view) return "Switch to Edit view to locate its renderer; Simulation view is independent.";
        if(id.starts_with("path.") && !c.path_available) return c.path_reason.empty() ? "Path tracing is unavailable in this build/device." : c.path_reason;
        if(c.inspector_draft || c.environment_draft) return "Apply or Revert the Scene draft first.";
        if(c.gizmo || c.navigating) return "Finish the active Edit Gizmo/navigation gesture first.";
        if(c.camera_popup) return "Apply or Cancel the Camera popup first.";
        if((id=="path.pause" || id=="path.reset") && !c.path_mode) return "Select Path trace in Viewport first.";
        if((id=="path.initial_candidates" || id=="path.temporal_reuse" || id=="path.spatial_reuse") && !c.restir) return "Select ReSTIR DI to edit reuse/candidate settings.";
        if((id=="path.time_seconds" || id=="path.cloud_albedo" || id=="path.cloud_anisotropy") && !c.scene_media) return "Select Scene media to edit medium settings.";
        if(id=="viewport.unlit_inspection" && c.path_mode) return "Unlit inspection applies to the raster renderer.";
        if(id.starts_with("viewport.voxel_") && c.path_mode) return "Voxel display settings apply to the raster renderer.";
    } else if(id.starts_with("simulation.") && s.section!="Next experiment") {
        if(c.gizmo) return "Finish the active Gizmo first.";
        if(id=="simulation.play" || id=="simulation.prepare") {
            if(c.simulation_run) return "Stop the active experiment before starting another.";
            if(!c.scene || c.inspector_draft || c.environment_draft) return "Open a scene and Apply/Revert Scene drafts first.";
        } else {
            if(!c.simulation_run) return "Start or Prepare an experiment first.";
            if(c.simulation_stopping) return "Wait for the experiment to finish stopping.";
            if((id=="simulation.step" || id=="simulation.resume" || id=="simulation.sample" || id=="simulation.export") && (!c.simulation_paused || c.simulation_fault)) return "Pause at a healthy completed step first.";
            if((id=="simulation.resume" || id=="simulation.step" || id=="simulation.cancel" || id=="simulation.pause") && c.simulation_terminal) return "The experiment has finished; Stop before restarting.";
            if(id=="simulation.pause" && (c.simulation_paused || c.simulation_fault)) return "Only a healthy running/initializing experiment can pause.";
        }
    }
    return {};
}
}
