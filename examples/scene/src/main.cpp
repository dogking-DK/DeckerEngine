#include <dk/scene/SceneIO.hpp>
#include <iostream>
#include <string_view>

namespace {
dk::Result<std::unique_ptr<dk::SceneDocument>> create(const std::filesystem::path& root)
{
    const auto asset = dk::AssetId::generate();
    if (!asset) { return std::unexpected(asset.error()); }
    const auto project = dk::Project::create(root, {"Scene CPU sample", "scene.json",
        {{*asset, dk::AssetKind::mesh, "mesh.bin"}}});
    if (!project) { return std::unexpected(project.error()); }
    auto document = dk::SceneDocument::create();
    if (!document) { return document; }
    auto& scene = **document;
    const auto parent = scene.create_entity();
    if (!parent) { return std::unexpected(parent.error()); }
    const auto child = scene.create_entity();
    if (!child) { return std::unexpected(child.error()); }
    auto result = scene.set_name(*parent, "父节点");
    if (!result) { return std::unexpected(result.error()); }
    result = scene.set_name(*child, "child");
    if (!result) { return std::unexpected(result.error()); }
    dk::Trsd local;
    local.translation = {10, 0, 0};
    local.scale = {2, 3, 1};
    result = scene.set_local_transform(*parent, local);
    if (!result) { return std::unexpected(result.error()); }
    local = {};
    local.translation = {1, 2, 3};
    result = scene.set_local_transform(*child, local);
    if (!result) { return std::unexpected(result.error()); }
    result = scene.set_parent(*child, *parent);
    if (!result) { return std::unexpected(result.error()); }
    result = scene.set_asset_references(*child, {{*asset, dk::AssetKind::mesh}});
    if (!result) { return std::unexpected(result.error()); }
    result = dk::save_scene(scene, *project);
    if (!result) { return std::unexpected(result.error()); }
    result = dk::save_project(*project, "project.json");
    if (!result) { return std::unexpected(result.error()); }
    return document;
}
dk::Result<std::unique_ptr<dk::SceneDocument>> load(const std::filesystem::path& root)
{
    const auto project = dk::Project::open(root, "project.json");
    if (!project) { return std::unexpected(project.error()); }
    return dk::load_scene(*project);
}
template<typename Character>
int entry(int argc, Character* argv[], std::basic_string_view<Character> create_command,
    std::basic_string_view<Character> load_command)
{
    if (argc != 3 || (std::basic_string_view<Character>{argv[1]} != create_command &&
        std::basic_string_view<Character>{argv[1]} != load_command)) {
        std::cerr << "Usage: dk-scene-demo <create|load> <existing-root-with-mesh.bin>\n";
        return 2;
    }
    try {
        const std::filesystem::path root{argv[2]};
        const auto scene = std::basic_string_view<Character>{argv[1]} == create_command ? create(root) : load(root);
        if (!scene) {
            std::cerr << dk::error_code_name(scene.error().code) << ": " << scene.error().message << '\n';
            for (const auto& item : scene.error().context) { std::cerr << "  " << item << '\n'; }
            return 1;
        }
        std::cout << "scene_id=" << (*scene)->id().to_string() << "\nrevision=" << (*scene)->revision()
            << "\nentities=" << (*scene)->entity_count() << "\ndirty=" << (*scene)->dirty() << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "internal_error: " << error.what() << '\n';
        return 1;
    }
}
}
#ifdef _WIN32
int wmain(int argc, wchar_t* argv[]) { return entry<wchar_t>(argc, argv, L"create", L"load"); }
#else
int main(int argc, char* argv[]) { return entry<char>(argc, argv, "create", "load"); }
#endif
