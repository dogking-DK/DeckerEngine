#pragma once
#include <dk/editor/Camera.hpp>
#include <imgui.h>
#include <map>
#include <string>

namespace dk::editor::detail {
class CameraSettingsUi final {
public:
    // Returns true while the popup owns input. A failed Apply retains the candidate.
    bool draw(Camera&,bool fixture,double aspect,std::map<std::string,ImVec2>& controls,const char* prefix);
    [[nodiscard]] bool pending() const { return open_; }
    void cancel() { cancel_=true; open_=false; }
private:
    CameraSettings draft_;
    std::string error_;
    bool open_=false,cancel_=false;
};
}
