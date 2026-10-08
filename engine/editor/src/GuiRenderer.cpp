#include "GuiRenderer.hpp"
#include "EditorSupport.hpp"
#include <imgui_impl_sdl3.h>
#include <imgui_impl_vulkan.h>
#include <array>
#include <cmath>
#include <SDL3/SDL_stdinc.h>
#include <dk/io/Path.hpp>

namespace dk::editor::detail {
namespace {
void vk_check(VkResult result) {
    if (result<0) throw std::runtime_error("ImGui Vulkan: "+std::string(graphics::vulkan_result_name(result)));
}
PFN_vkVoidFunction resolve(const char* name,void* user) {
    return static_cast<const graphics::Device*>(user)->instance_proc(name);
}
struct Draw {
    vk::ImageView color;
    vk::Extent2D extent;
    ImDrawData* data;
};
void record_draw(const vk::raii::CommandBuffer& command,void* user) {
    const auto& draw=*static_cast<Draw*>(user);
    vk::RenderingAttachmentInfo color;
    color.imageView=draw.color; color.imageLayout=vk::ImageLayout::eColorAttachmentOptimal;
    color.loadOp=vk::AttachmentLoadOp::eClear; color.storeOp=vk::AttachmentStoreOp::eStore;
    color.clearValue.color=vk::ClearColorValue{std::array<float,4>{0.025f,0.035f,0.05f,1}};
    vk::RenderingInfo rendering;
    rendering.renderArea=vk::Rect2D{{0,0},draw.extent}; rendering.layerCount=1;
    rendering.colorAttachmentCount=1; rendering.pColorAttachments=&color;
    command.beginRendering(rendering);
    ImGui_ImplVulkan_RenderDrawData(draw.data,static_cast<VkCommandBuffer>(*command));
    command.endRendering();
}
}
GuiRenderer::GuiRenderer(graphics::SubmissionQueue& queue,platform::Window& window) : queue_(queue) {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    try {
        auto& io=ImGui::GetIO();
        io.ConfigFlags|=ImGuiConfigFlags_DockingEnable;
        // No incidental imgui.ini write into the project root; layout lasts for this session.
        io.IniFilename=nullptr;
        io.ConfigDpiScaleFonts=true;
        io.ConfigDpiScaleViewports=true;
        // Use the local system UI font when available; no font asset is copied into the repository.
        const char* windows=SDL_getenv("WINDIR");
        if (!windows) windows=SDL_getenv("windir");
        if (windows) {
            auto directory=path_from_utf8(windows);
            if (directory) {
                const auto font=*directory/"Fonts/msyh.ttc";
                if (std::filesystem::is_regular_file(font)) {
                    const auto path=take(path_to_utf8(font));
                    io.Fonts->AddFontFromFileTTF(path.c_str(),16.0f);
                }
            }
        }
        ImGui::StyleColorsDark();
        auto& style=ImGui::GetStyle();
        style.WindowRounding=3; style.FrameRounding=3; style.GrabRounding=3;
        style.FramePadding={7,5}; style.ItemSpacing={8,7};
        style.Colors[ImGuiCol_WindowBg]={0.065f,0.078f,0.095f,1};
        style.Colors[ImGuiCol_Header]={0.13f,0.24f,0.30f,1};
        style.Colors[ImGuiCol_Button]={0.14f,0.28f,0.34f,1};
        style.Colors[ImGuiCol_CheckMark]={0.25f,0.78f,0.78f,1};
        style_srgb_=style;
        if (!ImGui_ImplSDL3_InitForVulkan(take(window.native_sdl_window()))) throw std::runtime_error("ImGui SDL3 initialization failed");
        platform_=true;
    } catch (...) {
        if (ImGui::GetIO().BackendPlatformUserData) ImGui_ImplSDL3_Shutdown();
        ImGui::DestroyContext();
        throw;
    }
}
GuiRenderer::~GuiRenderer() {
    shutdown_gpu();
    if (platform_) ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
}
bool GuiRenderer::compatible(const graphics::Frame& frame) const {
    return gpu_ && format_==frame.format() && images_==frame.image_count();
}
void GuiRenderer::shutdown_gpu() {
    if (ImGui::GetIO().BackendRendererUserData) ImGui_ImplVulkan_Shutdown();
    gpu_=false;
}
void GuiRenderer::initialize(const graphics::Frame& frame) {
    auto& device=queue_.device();
    if (frame.image_count()<2) throw std::runtime_error("ImGui requires at least two swapchain images");
    if (!ImGui_ImplVulkan_LoadFunctions(graphics::device_api_version,resolve,const_cast<graphics::Device*>(&device)))
        throw std::runtime_error("ImGui Vulkan function loading failed");
    ImGui::GetStyle()=style_srgb_;
    if (frame.format()==vk::Format::eB8G8R8A8Srgb || frame.format()==vk::Format::eR8G8B8A8Srgb) {
        for (auto& color : ImGui::GetStyle().Colors) {
            const auto linear=[](float v) { return v<=0.04045f ? v/12.92f : std::pow((v+0.055f)/1.055f,2.4f); };
            color.x=linear(color.x); color.y=linear(color.y); color.z=linear(color.z);
        }
    }
    const VkFormat format=static_cast<VkFormat>(frame.format());
    ImGui_ImplVulkan_InitInfo info{};
    info.ApiVersion=graphics::device_api_version;
    info.Instance=static_cast<VkInstance>(*device.instance());
    info.PhysicalDevice=static_cast<VkPhysicalDevice>(*device.physical_device());
    info.Device=device.native_device(); info.QueueFamily=device.queue_family();
    info.Queue=static_cast<VkQueue>(*device.queue());
    info.DescriptorPoolSize=64; info.MinImageCount=2; info.ImageCount=frame.image_count();
    info.UseDynamicRendering=true; info.MinAllocationSize=1024*1024; info.CheckVkResultFn=vk_check;
    info.PipelineInfoMain.PipelineRenderingCreateInfo.sType=VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    info.PipelineInfoMain.PipelineRenderingCreateInfo.colorAttachmentCount=1;
    info.PipelineInfoMain.PipelineRenderingCreateInfo.pColorAttachmentFormats=&format;
    if (!ImGui_ImplVulkan_Init(&info)) throw std::runtime_error("ImGui Vulkan initialization failed");
    gpu_=true; format_=frame.format(); images_=frame.image_count();
}
void GuiRenderer::new_frame(const std::function<void()>& input) {
    ImGui_ImplVulkan_NewFrame(); ImGui_ImplSDL3_NewFrame();
    if (input) input();
    ImGui::NewFrame();
}
void GuiRenderer::record(graphics::Frame& frame,const graphics::Image* texture,const graphics::ImageView* view) {
    auto& batch=frame.commands();
    auto color=graphics::image_use(frame.color(),vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        vk::AccessFlagBits2::eColorAttachmentRead | vk::AccessFlagBits2::eColorAttachmentWrite,vk::ImageLayout::eColorAttachmentOptimal);
    color.full_overwrite=true;
    std::array<graphics::ResourceUse,2> uses{color,{}};
    std::size_t count=1;
    check(batch.retain(frame.color_view()));
    if (texture && view) {
        uses[1]=graphics::image_use(*texture,vk::PipelineStageFlagBits2::eFragmentShader,
            vk::AccessFlagBits2::eShaderSampledRead,vk::ImageLayout::eShaderReadOnlyOptimal);
        check(batch.retain(*view)); count=2;
    }
    Draw draw{frame.color_view().handle(),frame.extent(),ImGui::GetDrawData()};
    check(batch.unsafe_record(std::span{uses.data(),count},std::span{uses.data(),count},record_draw,&draw));
}
}
