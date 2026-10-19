#pragma once
#include <dk/editor/Camera.hpp>

namespace dk::editor {
struct NavigationInput {
    Vec2d look=Vec2d::Zero(); // logical pixels; never multiplied by dt
    Vec3d move=Vec3d::Zero(); // right, world up, forward; each axis in [-1,1]
    double seconds=0,wheel=0;
    bool fast=false,slow=false;
};
// UI owns capture and the monotonic clock. This model has no window dependency.
class CameraNavigation final {
public:
    void begin() { active_=true; first_=true; }
    void cancel() { active_=false; first_=true; }
    [[nodiscard]] bool active() const { return active_; }
    [[nodiscard]] Result<void> update(Camera&,const NavigationInput&);
private:
    bool active_=false,first_=true;
};
}
