#include "Viewport.hpp"
#include "EditorSupport.hpp"
#include <imgui_impl_vulkan.h>
#include <cmath>
#include <vector>

namespace dk::editor::detail {
namespace {
render::ViewDescription camera(std::uint32_t width,std::uint32_t height,std::uint64_t frame,bool fixture) {
    render::ViewDescription result{width,height,frame};
    const Vec3d eye=fixture ? Vec3d{0,0,-2} : Vec3d{8,1.8,0};
    const Vec3d target=fixture ? Vec3d{0,0,0} : Vec3d{-4,2,0};
    const Vec3d forward=(target-eye).normalized(),right=forward.cross(Vec3d::UnitY()).normalized(),up=right.cross(forward);
    Mat4d world=Mat4d::Identity();
    world.block<3,1>(0,0)=right; world.block<3,1>(0,1)=up;
    world.block<3,1>(0,2)=-forward; world.block<3,1>(0,3)=eye;
    result.camera_world=take(Transformd::from_matrix(world));
    constexpr double near=0.05,far=100;
    const double f=1/std::tan(65*3.14159265358979323846/360);
    result.projection=Mat4d::Zero(); result.projection(0,0)=f/(double(width)/height);
    result.projection(1,1)=-f; result.projection(2,2)=far/(near-far);
    result.projection(2,3)=far*near/(near-far); result.projection(3,2)=-1;
    return result;
}
}
Viewport::Viewport(memory::ResourceHandle heap,graphics::SubmissionQueue& queue)
    : heap_(heap),queue_(queue),pipeline_(take(render::ScenePipeline::create(heap,queue,DK_EDITOR_SHADER_DIR))) {}
Viewport::~Viewport() { release_texture(); }
void Viewport::release_texture() {
    if (descriptor_) ImGui_ImplVulkan_RemoveTexture(descriptor_);
    descriptor_=VK_NULL_HANDLE;
    attempted_.reset();
}
void Viewport::update(const SceneReadSnapshot& snapshot,std::uint32_t width,std::uint32_t height,bool srgb,bool fixture) {
    const Key key{snapshot.state.document_id,snapshot.state.revision,width,height,srgb};
    if (attempted_==key) return;
    attempted_=key;
    try {
        if (asset_session_!=key.session) {
            auto source=take(render::DiskScene::load_snapshot(heap_,snapshot.project,snapshot.scene,{GltfImportProfile::unlit_preview}));
            auto candidate=take(source.upload(queue_));
            assets_=std::move(candidate);
            asset_session_=key.session;
        }
        auto scene=take(render::RenderScene::extract(heap_,snapshot.scene));
        auto render_view=take(render::RenderView::create(std::move(scene),camera(width,height,info_.frame+1,fixture)));
        auto frame=take(pipeline_.render(queue_,render_view,assets_,{{0.025f,0.035f,0.05f},1,false}));
        if (!take(frame.wait(queue_))) throw std::runtime_error("Viewport render timed out");
        std::vector<std::byte> pixels(static_cast<std::size_t>(width)*height*4);
        check(frame.read_rgba8(pixels));
        // ScenePipeline returns encoded sRGB. An sRGB view decodes before the sRGB swapchain encodes.
        graphics::ImageDesc desc{width,height,srgb ? vk::Format::eR8G8B8A8Srgb : vk::Format::eR8G8B8A8Unorm};
        auto image=take(queue_.create_image(desc));
        auto view=take(queue_.resources().create_view(image));
        auto batch=take(queue_.begin());
        check(batch.upload(image,pixels,{0,0,0,0,width,height}));
        check(batch.transition(image,vk::ImageLayout::eShaderReadOnlyOptimal));
        auto submission=take(queue_.submit(std::move(batch)));
        if (!take(queue_.wait(submission))) throw std::runtime_error("Viewport upload timed out");
        auto descriptor=ImGui_ImplVulkan_AddTexture(static_cast<VkImageView>(view.handle()),VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        if (!descriptor) throw std::runtime_error("Viewport texture registration failed");
        release_texture();
        image_=std::move(image); view_=std::move(view); descriptor_=descriptor;
        pixel_signature_=14695981039346656037ULL;
        for (const auto byte : pixels) { pixel_signature_^=std::to_integer<unsigned char>(byte); pixel_signature_*=1099511628211ULL; }
        info_=frame.info(); published_session_=key.session; attempted_=key; error_.clear();
    } catch (const std::exception& error) { error_=error.what(); }
}
}
