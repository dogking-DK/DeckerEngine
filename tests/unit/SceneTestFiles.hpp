#pragma once
#include <dk/core/StableId.hpp>
#include <dk/io/File.hpp>
#include <dk/io/Path.hpp>
#include <catch2/catch_test_macros.hpp>

struct SceneTestFiles {
    std::filesystem::path root;
    SceneTestFiles()
    {
        const auto suffix = dk::EntityId::generate();
        REQUIRE(suffix.has_value());
        root = std::filesystem::current_path() / "test-artifacts" / ("scene-" + suffix->to_string()) / *dk::path_from_utf8("工程😀");
        REQUIRE(std::filesystem::create_directories(root));
    }
    ~SceneTestFiles() { std::error_code error; std::filesystem::remove_all(root.parent_path(), error); }
    void write(const std::filesystem::path& relative, std::string_view text) const
    {
        const auto path = root / relative;
        std::filesystem::create_directories(path.parent_path());
        REQUIRE(dk::write_file_bytes(path, std::as_bytes(std::span{text.data(), text.size()})).has_value());
    }
};
