#include <dk/services/AssetService.hpp>
#include <dk/memory/MemorySystem.hpp>
#include <dk/io/File.hpp>
#include "AssetPersistenceInternal.hpp"
#include <cstdlib>
#include <iostream>

namespace {
template<class T> T take(dk::Result<T> result)
{ if (!result) { throw std::runtime_error(result.error().message); } return std::move(*result); }
void take(dk::Result<void> result) { if (!result) { throw std::runtime_error(result.error().message); } }
void write(const std::filesystem::path& path, std::string_view text)
{ take(dk::write_file_bytes(path, std::as_bytes(std::span{text.data(), text.size()}))); }
}
int wmain(int argc, wchar_t** argv)
{
    if (argc != 5) { return 2; }
    try {
        auto memory = std::move(dk::memory::MemorySystem::create().value());
        auto heap = memory.create_heap({"assets-probe", dk::memory::DomainCategory::assets}).value();
        dk::memory::ThreadContext context{memory}; dk::memory::ExecutionScope scope{context, heap};
        const std::filesystem::path root{argv[1]}; const std::wstring_view mode{argv[2]}, kind{argv[3]}, point{argv[4]};
        const bool rename = kind == L"rename";
        if (mode == L"seed") {
            std::filesystem::create_directories(root / "assets");
            write(root / "assets/old.gltf", "original source");
            write(root / "project.json", take(dk::serialize_project({"restart", "scene.json", {}})));
            if (rename) {
                auto service = take(dk::AssetService::open(root, "project.json"));
                take(service.register_source(service.catalog().guard(), {"assets/old.gltf", {}, {}, dk::MissingMetaPolicy::create_or_adopt}));
            }
            const auto manifest = take(dk::read_file_bytes(root / "project.json")); take(dk::write_file_bytes(root / "expected-project.json", manifest));
            if (rename) { const auto meta = take(dk::read_file_bytes(root / "assets/old.gltf.meta")); take(dk::write_file_bytes(root / "expected-meta.json", meta)); }
            return 0;
        }
        if (mode == L"crash") {
            auto service = take(dk::AssetService::open(root, "project.json"));
            using namespace dk::asset_detail;
            const auto at = point == L"source" ? OperationStep::source : point == L"meta" ? OperationStep::meta
                : point == L"manifest" ? OperationStep::manifest : OperationStep::finish;
            OperationHook hook = [at](OperationStep step) -> dk::Result<void> { if (step == at) { std::_Exit(73); } return {}; };
            ScopedOperationHook injection{hook};
            if (rename) { take(service.rename_source(service.catalog().guard(), "assets/old.gltf", "assets/new.gltf")); }
            else { take(service.register_source(service.catalog().guard(), {"assets/old.gltf", {}, {}, dk::MissingMetaPolicy::create_or_adopt})); }
            return 3;
        }
        if (mode == L"recover") {
            if (dk::AssetService::open(root, "project.json")) { return 4; }
            take(dk::recover_asset_operations(root)); take(dk::recover_asset_operations(root));
            auto service = take(dk::AssetService::open(root, "project.json"));
            if (take(dk::read_file_bytes(root / "project.json")) != take(dk::read_file_bytes(root / "expected-project.json"))) { return 5; }
            if (rename && take(dk::read_file_bytes(root / "assets/old.gltf.meta")) != take(dk::read_file_bytes(root / "expected-meta.json"))) { return 6; }
            if (!rename && std::filesystem::exists(root / "assets/old.gltf.meta")) { return 7; }
            if (!std::filesystem::exists(root / "assets/old.gltf") || std::filesystem::exists(root / "assets/new.gltf")
                || std::filesystem::exists(root / "assets/new.gltf.meta")) { return 8; }
            return service.catalog().guard().revision == 0 ? 0 : 9;
        }
        return 10;
    } catch (const std::exception& error) { std::cerr << error.what(); return 11; }
}
