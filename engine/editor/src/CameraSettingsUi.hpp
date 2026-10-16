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
private:
    CameraSettings draft_;
    std::string error_;
};
}
