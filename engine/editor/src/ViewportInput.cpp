#include "ViewportInput.hpp"
#include "EditorSupport.hpp"
#include <Eigen/LU>
#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>

namespace dk::editor::detail {
namespace {
Vec2d vector(ImVec2 v) { return {v.x,v.y}; }
ImVec2 point(const Vec2d& v) { return {static_cast<float>(v.x()),static_cast<float>(v.y())}; }
void mark(std::map<std::string,ImVec2>& controls,const char* name) {
    controls[name]=point((vector(ImGui::GetItemRectMin())+vector(ImGui::GetItemRectMax()))/2);
}
double segment_distance(const Vec2d& p,const Vec2d& a,const Vec2d& b) {
    const Vec2d d=b-a;
    if (d.squaredNorm()<1) return (p-a).norm();
    return (p-(a+d*std::clamp((p-a).dot(d)/d.squaredNorm(),0.0,1.0))).norm();
}
std::optional<double> angle(const geometry::Ray& ray,const Vec3d& pivot,const Mat3d& basis,int axis) {
    const int u=(axis+1)%3,v=(axis+2)%3;
    const Vec3d normal=basis.col(u).cross(basis.col(v)).normalized();
    const double denominator=normal.dot(ray.direction);
    if (std::abs(denominator)<1e-8) return {};
    const double t=normal.dot(pivot-ray.origin)/denominator;
    if (t<ray.minimum || t>ray.maximum) return {};
    const Vec3d local=basis.inverse()*(ray.origin+t*ray.direction-pivot);
    if (!local.allFinite() || std::hypot(local[u],local[v])<1e-8) return {};
    return std::atan2(local[v],local[u]);
}
}
void ViewportInput::cancel() { if (edit_) { edit_.reset(); ++revision_; } navigation_=-1; }
void ViewportInput::toolbar(Workspace& model,Viewport& preview,std::map<std::string,ImVec2>& controls) {
    ImGui::BeginDisabled(active());
    if (ImGui::RadioButton("Move",mode_==Mode::translate)) mode_=Mode::translate; mark(controls,"mode_move"); ImGui::SameLine();
    if (ImGui::RadioButton("Rotate",mode_==Mode::rotate)) mode_=Mode::rotate; mark(controls,"mode_rotate"); ImGui::SameLine();
    if (ImGui::RadioButton("Scale",mode_==Mode::scale)) mode_=Mode::scale; mark(controls,"mode_scale"); ImGui::SameLine();
    if (ImGui::SmallButton("Reset")) camera_.reset(fixture_); mark(controls,"camera_reset");
    ImGui::SameLine();
    ImGui::BeginDisabled(!model.selection());
    if (ImGui::SmallButton("Frame") && model.snapshot()) camera_.frame(preview.bounds(*model.selection(),model.snapshot()->scene),
        preview.info().height ? double(preview.info().width)/preview.info().height : 1);
    mark(controls,"camera_frame"); ImGui::EndDisabled(); ImGui::EndDisabled();
    ImGui::TextDisabled(active() ? "Transform preview | release: commit | Esc: cancel" : "Local axes | RMB: orbit | MMB: pan | wheel: zoom | F: frame | Home: reset");
}
void ViewportInput::image(Workspace& model,Viewport& preview,std::map<std::string,ImVec2>& controls,const Report& report) {
    const Vec2d minimum=vector(ImGui::GetItemRectMin()),size=vector(ImGui::GetItemRectSize());
    if (active() && (minimum!=capture_minimum_ || size!=capture_size_)) { cancel(); return; }
    const double aspect=double(preview.info().width)/preview.info().height;
    auto& io=ImGui::GetIO(); const Vec2d mouse=vector(io.MousePos),uv=(mouse-minimum).cwiseQuotient(size);
    controls["viewport"]=point(minimum+size/2); controls["viewport_empty"]=point(minimum+Vec2d{size.x()*0.03,size.y()*0.03});
    const bool modal=ImGui::IsPopupOpen(nullptr,ImGuiPopupFlags_AnyPopupId|ImGuiPopupFlags_AnyPopupLevel);
    if (io.AppFocusLost || modal || !model.snapshot() || !preview.current(model.snapshot()->state) ||
        !preview.error().empty()) { cancel(); return; }
    if (ImGui::IsKeyPressed(ImGuiKey_Escape,false)) { cancel(); return; }
    const bool hovered=ImGui::IsItemHovered();
    if (!active() && !io.WantTextInput) {
        if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) navigation_=ImGuiMouseButton_Right;
        if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Middle)) navigation_=ImGuiMouseButton_Middle;
        if (navigation_>=0 && !ImGui::IsMouseDown(navigation_)) navigation_=-1;
        if (navigation_==ImGuiMouseButton_Right && !ImGui::IsMouseClicked(navigation_)) camera_.orbit(io.MouseDelta.x,io.MouseDelta.y);
        if (navigation_==ImGuiMouseButton_Middle && !ImGui::IsMouseClicked(navigation_)) camera_.pan(io.MouseDelta.x,io.MouseDelta.y,size.y());
        if (hovered && io.MouseWheel!=0) camera_.dolly(io.MouseWheel);
        if (hovered && ImGui::IsKeyPressed(ImGuiKey_Home,false)) camera_.reset(fixture_);
        if (hovered && ImGui::IsKeyPressed(ImGuiKey_F,false) && model.selection())
            camera_.frame(preview.bounds(*model.selection(),model.snapshot()->scene),aspect);
    }
    if (!preview.camera_current(camera_) || navigation_>=0) return;
    auto* draw=ImGui::GetWindowDrawList();
    draw->PushClipRect(point(minimum),point(minimum+size),true);
    const auto project=[&](const Vec3d& p) -> std::optional<Vec2d> {
        auto projected=camera_.project(p,aspect); if (!projected) return {}; return minimum+projected->cwiseProduct(size);
    };
    int hovered_axis=-1; double closest=9;
    std::optional<TransformEdit> base;
    if (model.selection()) {
        const auto bounds=preview.bounds(*model.selection(),model.snapshot()->scene);
        if (!bounds.empty()) {
            std::array<std::optional<Vec2d>,8> corners;
            for (int c=0;c<8;++c) corners[c]=project({c&1 ? bounds.maximum.x():bounds.minimum.x(),c&2 ? bounds.maximum.y():bounds.minimum.y(),c&4 ? bounds.maximum.z():bounds.minimum.z()});
            for (int c=0;c<8;++c) for (int bit=1;bit<=4;bit*=2) if (!(c&bit) && corners[c] && corners[c|bit])
                draw->AddLine(point(*corners[c]),point(*corners[c|bit]),IM_COL32(240,190,50,220),1.5f);
        }
        auto candidate=model.begin_transform();
        if (candidate) base=active() ? edit_ : std::optional{*candidate};
    }
    if (base) {
        auto world=base->world();
        if (world) {
            const Vec3d pivot=world->matrix().block<3,1>(0,3);
            const Mat3d basis=base->parent.matrix().block<3,3>(0,0)*base->original.rotation.normalized().toRotationMatrix();
            const auto center=project(pivot);
            if (center) controls["gizmo_center"]=point(*center);
            const double units=camera_.units_per_pixel(pivot,size.y());
            constexpr std::array<ImU32,3> colors{IM_COL32(245,80,75,255),IM_COL32(90,225,100,255),IM_COL32(85,145,255,255)};
            for (int axis=0;center && axis<3;++axis) {
                const auto color=active() && axis==axis_ ? IM_COL32(255,225,50,255) : colors[axis];
                const Vec3d direction=basis.col(axis).normalized();
                auto end=project(pivot+direction*units*80);
                if (mode_!=Mode::rotate && end && (*end-*center).norm()>15) {
                    draw->AddLine(point(*center),point(*end),color,3);
                    if (mode_==Mode::scale) draw->AddRectFilled(point(*end-Vec2d{5,5}),point(*end+Vec2d{5,5}),color);
                    else draw->AddCircleFilled(point(*end),5,color);
                    draw->AddText(point(*end+Vec2d{5,-12}),color,axis==0 ? "X":axis==1 ? "Y":"Z");
                    const Vec2d handle=*center+(*end-*center)*0.75;
                    controls["gizmo_"+std::to_string(axis)]=point(handle);
                    const double distance=segment_distance(mouse,*center+(*end-*center)*0.25,*end);
                    if (distance<closest) { closest=distance; hovered_axis=axis; }
                } else if (mode_==Mode::rotate) {
                    const int u=(axis+1)%3,v=(axis+2)%3;
                    const double radius=units*65/std::max(basis.col(u).norm(),basis.col(v).norm());
                    std::optional<Vec2d> previous;
                    for (int i=0;i<=64;++i) {
                        const double a=2*std::numbers::pi*i/64;
                        auto p=project(pivot+radius*(std::cos(a)*basis.col(u)+std::sin(a)*basis.col(v)));
                        if (p && previous) {
                            draw->AddLine(point(*previous),point(*p),color,2.5f);
                            const double distance=segment_distance(mouse,*previous,*p);
                            if (distance<closest) { closest=distance; hovered_axis=axis; }
                        }
                        if (p && i==8) controls["gizmo_"+std::to_string(axis)]=point(*p);
                        previous=p;
                    }
                }
            }
            if (!active() && hovered && hovered_axis>=0 && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                auto capture=model.begin_transform();
                if (!capture) report("Gizmo",std::unexpected(capture.error()));
                else {
                    axis_=hovered_axis; pivot_=pivot; basis_=basis; axis_world_=basis.col(axis_).normalized();
                    start_mouse_=mouse; units_=units;
                    const auto projected=project(pivot+axis_world_*units*80);
                    screen_axis_=projected && center ? Vec2d{(*projected-*center)/80} : Vec2d::Zero();
                    auto ray=camera_.ray(uv,aspect);
                    const auto a=ray ? angle(*ray,pivot_,basis_,axis_) : std::nullopt;
                    if (mode_!=Mode::rotate || a) {
                        edit_=*capture; start_angle_=a.value_or(0); capture_minimum_=minimum; capture_size_=size; ++revision_;
                    }
                }
            }
        }
    }
    draw->PopClipRect();
    if (active()) {
        // A captured drag may leave the image. Clamp only for the rotation-plane ray.
        Result<void> result;
        if (mode_==Mode::rotate) {
            auto ray=camera_.ray(uv.cwiseMax(0.0).cwiseMin(1.0),aspect);
            const auto a=ray ? angle(*ray,pivot_,basis_,axis_) : std::nullopt;
            if (a) result=edit_->rotate(static_cast<unsigned>(axis_),std::remainder(*a-start_angle_,2*std::numbers::pi));
        } else {
            const double delta=screen_axis_.squaredNorm()>1e-6 ? (mouse-start_mouse_).dot(screen_axis_)/screen_axis_.squaredNorm() : 0;
            if (mode_==Mode::translate) result=edit_->translate(axis_world_*(delta*units_));
            else result=edit_->scale(static_cast<unsigned>(axis_),std::clamp(1+delta/80,0.01,100.0));
        }
        if (!result) { report("Gizmo preview",std::move(result)); cancel(); }
        else {
            if (io.MouseDelta.x!=0 || io.MouseDelta.y!=0) ++revision_;
            if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) { report("Gizmo commit",model.commit_transform(*edit_)); cancel(); }
            else if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) cancel();
        }
    } else if (hovered && !model.pending() && hovered_axis<0 && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        auto selection=preview.pick(uv);
        if (!selection) report("Pick",std::unexpected(selection.error()));
        else report("Pick",model.select(*selection));
    }
}
}
