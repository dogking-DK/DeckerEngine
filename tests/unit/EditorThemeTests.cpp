#include <catch2/catch_test_macros.hpp>
#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_spectrum.h>
#include <algorithm>
#include <cmath>

namespace {
struct ThemeContext {
    ThemeContext() {
        ImGui::CreateContext();
        ImGui::GetIO().IniFilename=nullptr;
        ImGui::Spectrum::SetLinearColorOutput(false);
        ImGui::Spectrum::StyleColorsSpectrum(true);
    }
    ~ThemeContext() { ImGui::DestroyContext(); }
};
}

TEST_CASE("theme palette survives output format and theme changes", "[editor][theme]") {
    ThemeContext context;
    const ImU32 sample=IM_COL32(128,128,128,77);
    REQUIRE(ImGui::Spectrum::ColorToOutput(sample)==sample);
    ImGui::Spectrum::SetLinearColorOutput(true);
    const auto linear=ImGui::ColorConvertU32ToFloat4(ImGui::Spectrum::ColorToOutput(sample));
    REQUIRE(std::abs(linear.x-0.216f)<0.003f);
    REQUIRE(std::abs(linear.w-77.0f/255.0f)<0.001f);
    const auto dark=ImGui::GetStyle().Colors[ImGuiCol_WindowBg];
    for (int i=0;i<3;++i) {
        ImGui::Spectrum::SetLinearColorOutput(false);
        REQUIRE(ImGui::Spectrum::ColorToOutput(sample)==sample);
        ImGui::Spectrum::StyleColorsSpectrum(false);
        REQUIRE(ImGui::GetStyle().Colors[ImGuiCol_WindowBg].x>0.9f);
        ImGui::Spectrum::StyleColorsSpectrum(true);
        REQUIRE(ImGui::GetStyle().Colors[ImGuiCol_WindowBg].x==dark.x);
        ImGui::Spectrum::SetLinearColorOutput(true);
        REQUIRE(ImGui::ColorConvertU32ToFloat4(ImGui::Spectrum::ColorToOutput(sample)).x==linear.x);
    }
    REQUIRE(ImGui::GetStyle().Colors[ImGuiCol_DockingPreview].w>0.0f);
    REQUIRE(ImGui::GetStyle().Colors[ImGuiCol_TableHeaderBg].x<0.3f);
}

TEST_CASE("theme controls honor disabled alpha and emit finite geometry", "[editor][theme]") {
    ThemeContext context;
    auto& io=ImGui::GetIO();
    io.DisplaySize={800,600}; io.DeltaTime=1.0f/60.0f;
    unsigned char* pixels=nullptr; int width=0,height=0;
    io.Fonts->GetTexDataAsRGBA32(&pixels,&width,&height);
    auto& style=ImGui::GetStyle();
    style.Alpha=0.8f; style.DisabledAlpha=0.5f;
    ImGui::Spectrum::SetLinearColorOutput(true);
    ImGui::NewFrame();
    ImGui::SetNextWindowPos({0,0}); ImGui::SetNextWindowSize({700,550});
    ImGui::Begin("Theme controls",nullptr,ImGuiWindowFlags_NoSavedSettings);
    ImGui::BeginDisabled();
    auto* draw=ImGui::GetWindowDrawList();
    const int first=draw->VtxBuffer.Size;
    bool checked=true,unchecked=false;
    ImGui::Checkbox("Checked",&checked);
    ImGui::Checkbox("Unchecked",&unchecked);
    ImGui::PushItemFlag(ImGuiItemFlags_MixedValue,true);
    ImGui::Checkbox("Mixed",&unchecked);
    ImGui::PopItemFlag();
    ImGui::RadioButton("Active",true);
    ImGui::RadioButton("Inactive",false);
    ImGui::Selectable("Selected",true);
    ImGui::CollapsingHeader("Header");
    const int last=draw->VtxBuffer.Size;
    ImGui::EndDisabled();
    ImGui::End();
    ImGui::Render();
    REQUIRE(last>first);
    unsigned int max_alpha=0;
    for (int i=first;i<last;++i) {
        const auto& vertex=draw->VtxBuffer[i];
        REQUIRE(std::isfinite(vertex.pos.x));
        REQUIRE(std::isfinite(vertex.pos.y));
        max_alpha=std::max(max_alpha,(vertex.col>>IM_COL32_A_SHIFT)&0xffU);
    }
    REQUIRE(max_alpha>0);
    REQUIRE(max_alpha<=103); // 0.8 global * 0.5 disabled * 255, allowing rounding.
}
