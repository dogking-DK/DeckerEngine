#pragma once
#include "Viewport.hpp"
#include "ViewportInput.hpp"
#include <map>

namespace dk::editor::detail {
enum class PendingAction { none, open, close };
class WorkbenchUi final {
public:
    explicit WorkbenchUi(Workspace& model,bool fixture);
    void draw(Viewport&,bool srgb);
    void request_close();
    void cancel_interaction() { input_.cancel(); }
    [[nodiscard]] bool closing() const { return closing_; }
    [[nodiscard]] const std::map<std::string,ImVec2>& controls() const { return controls_; }
    [[nodiscard]] unsigned errors() const { return errors_; }
    [[nodiscard]] const ViewportInput& input() const { return input_; }
private:
    void mark(const char* id);
    bool report(const char*,Result<void>);
    void request(PendingAction);
    void perform();
    void toolbar();
    void hierarchy();
    void inspector();
    void assets();
    void console();
    void confirmation();
    Workspace& model_;
    ViewportInput input_;
    std::string manifest_;
    std::map<std::string,ImVec2> controls_;
    std::vector<std::string> log_;
    std::string viewport_error_;
    PendingAction pending_ = PendingAction::none;
    bool popup_ = false,closing_ = false,layout_ = false,scroll_log_ = true;
    unsigned errors_ = 0;
};
}
