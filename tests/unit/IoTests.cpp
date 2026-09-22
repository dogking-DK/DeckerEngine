#include <dk/core/StableId.hpp>
#include <dk/io/File.hpp>
#include <dk/io/Path.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <limits>
#include <string>
#include <system_error>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#endif

namespace {

namespace fs = std::filesystem;

fs::path utf8_path(std::string_view text)
{
    auto result = dk::path_from_utf8(text);
    REQUIRE(result.has_value());
    return *result;
}

struct TestDirectory {
    fs::path root;

    TestDirectory()
    {
        const auto id = dk::AssetId::generate();
        REQUIRE(id.has_value());
        root = fs::current_path() / "test-artifacts" / DK_TEST_CONFIG / "io" / id->to_string();
        REQUIRE(fs::create_directories(root));
    }
    // Preserve artifacts inside the build tree for failure inspection.
};

struct CurrentPathGuard {
    fs::path original = fs::current_path();
    ~CurrentPathGuard()
    {
        std::error_code ignored;
        fs::current_path(original, ignored);
    }
};

dk::ByteBuffer patterned_bytes(std::size_t size)
{
    dk::ByteBuffer bytes(size);
    for (std::size_t index = 0; index < size; ++index) {
        bytes[index] = static_cast<std::byte>(index % 256U);
    }
    return bytes;
}

template <typename T>
void require_error(const dk::Result<T>& result, dk::ErrorCode code)
{
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().code == code);
    REQUIRE_FALSE(result.error().message.empty());
    REQUIRE_FALSE(result.error().context.empty());
}

} // namespace

TEST_CASE("UTF-8 paths preserve ASCII CJK and supplementary characters", "[io][path]")
{
    const std::array<std::string_view, 3> paths{"assets/data.bin", "资源/中文 文件.bin", "资源/📦.bin"};
    for (const auto text : paths) {
        const auto path = dk::path_from_utf8(text);
        REQUIRE(path.has_value());
        const auto restored = dk::path_to_utf8(*path);
        REQUIRE(restored.has_value());
        REQUIRE(*restored == text);
    }
#ifdef _WIN32
    REQUIRE(*dk::path_to_utf8(fs::path{L"assets\\data.bin"}) == "assets/data.bin");
#endif
}

TEST_CASE("UTF-8 paths reject invalid sequences empty input and embedded NUL", "[io][path]")
{
    const std::array<std::string, 10> invalid{
        "", "\x80", "\xc0\xaf", "\xe0\x80\xaf", "\xed\xa0\x80",
        "\xf4\x90\x80\x80", "\xf5\x80\x80\x80", "\xe4\xb8", "\xe4x\x80",
        std::string{"ok\0hidden", 9}};
    for (const auto& text : invalid) {
        require_error(dk::path_from_utf8(text), dk::ErrorCode::invalid_argument);
    }
    require_error(dk::path_to_utf8(fs::path{}), dk::ErrorCode::invalid_argument);
    fs::path::string_type native{fs::path{"ok"}.native()};
    native.push_back(fs::path::value_type{});
    native += fs::path{"hidden"}.native();
    require_error(dk::path_to_utf8(fs::path{native}), dk::ErrorCode::invalid_argument);
#ifdef _WIN32
    require_error(dk::path_to_utf8(fs::path{std::wstring(1, static_cast<wchar_t>(0xd800))}),
        dk::ErrorCode::invalid_argument);
    require_error(dk::path_to_utf8(fs::path{std::wstring(1, static_cast<wchar_t>(0xdc00))}),
        dk::ErrorCode::invalid_argument);
#else
    require_error(dk::path_to_utf8(fs::path{"\xff"}), dk::ErrorCode::invalid_argument);
#endif
}

TEST_CASE("project root requires an existing directory", "[io][path]")
{
    const TestDirectory directory;
    require_error(dk::ProjectPaths::create({}), dk::ErrorCode::invalid_argument);
    require_error(dk::ProjectPaths::create(directory.root / "missing"), dk::ErrorCode::not_found);
    const auto file = directory.root / "file.bin";
    REQUIRE(dk::write_file_bytes(file, {}).has_value());
    require_error(dk::ProjectPaths::create(file), dk::ErrorCode::invalid_argument);
    const auto project = dk::ProjectPaths::create(directory.root / ".");
    REQUIRE(project.has_value());
    REQUIRE(project->root().is_absolute());
    REQUIRE(project->root() == fs::canonical(directory.root));
}

TEST_CASE("project paths normalize relative paths without requiring a child to exist", "[io][path]")
{
    const TestDirectory directory;
    const auto project = dk::ProjectPaths::create(directory.root);
    REQUIRE(project.has_value());
    const auto resolved = project->resolve(utf8_path("资源/./旧/../📦.bin"));
    REQUIRE(resolved.has_value());
    REQUIRE(*resolved == project->root() / utf8_path("资源/📦.bin"));
    REQUIRE_FALSE(fs::exists(*resolved));
    const auto root = project->resolve(".");
    REQUIRE(root.has_value());
    REQUIRE(*root == project->root());
    const auto canceled = project->resolve("assets/..");
    REQUIRE(canceled.has_value());
    REQUIRE(*canceled == project->root());
}

TEST_CASE("project paths reject rooted paths and lexical root escape", "[io][path]")
{
    const TestDirectory directory;
    const auto project = dk::ProjectPaths::create(directory.root);
    REQUIRE(project.has_value());
    const std::array<fs::path, 5> invalid{
        fs::path{}, "../outside.bin", "assets/../../outside.bin", directory.root, "/rooted.bin"};
    for (const auto& path : invalid) {
        require_error(project->resolve(path), dk::ErrorCode::invalid_argument);
    }
#ifdef _WIN32
    require_error(project->resolve("C:relative.bin"), dk::ErrorCode::invalid_argument);
    require_error(project->resolve("\\\\server\\share\\file.bin"), dk::ErrorCode::invalid_argument);
#endif
}

TEST_CASE("project paths capture the root independently of later working directory changes", "[io][path]")
{
    const TestDirectory directory;
    REQUIRE(fs::create_directories(directory.root / "project"));
    REQUIRE(fs::create_directories(directory.root / "elsewhere"));
    const CurrentPathGuard guard;
    fs::current_path(directory.root);
    const auto project = dk::ProjectPaths::create("project");
    REQUIRE(project.has_value());
    fs::current_path(directory.root / "elsewhere");
    const auto resolved = project->resolve("data.bin");
    REQUIRE(resolved.has_value());
    REQUIRE(*resolved == directory.root / "project" / "data.bin");
}

TEST_CASE("binary files roundtrip all byte values through a Unicode path", "[io][file]")
{
    const TestDirectory directory;
    const auto folder = directory.root / utf8_path("资源 📦");
    REQUIRE(fs::create_directory(folder));
    const auto project = dk::ProjectPaths::create(directory.root);
    REQUIRE(project.has_value());
    const auto file = project->resolve(utf8_path("资源 📦/中文文件.bin"));
    REQUIRE(file.has_value());
    const auto bytes = patterned_bytes(256);
    REQUIRE(dk::write_file_bytes(*file, bytes).has_value());
    const auto restored = dk::read_file_bytes(*file);
    REQUIRE(restored.has_value());
    REQUIRE(*restored == bytes);
}

TEST_CASE("empty binary files are valid even with a zero read limit", "[io][file]")
{
    const TestDirectory directory;
    const auto file = directory.root / "empty.bin";
    REQUIRE(dk::write_file_bytes(file, {}).has_value());
    const auto bytes = dk::read_file_bytes(file, 0);
    REQUIRE(bytes.has_value());
    REQUIRE(bytes->empty());
    REQUIRE(fs::file_size(file) == 0);
}

TEST_CASE("binary reads enforce exact size limits across chunk boundaries", "[io][file]")
{
    const TestDirectory directory;
    const auto file = directory.root / "chunked.bin";
    for (const auto size : {std::size_t{1}, std::size_t{65536}, std::size_t{150000}}) {
        const auto bytes = patterned_bytes(size);
        REQUIRE(dk::write_file_bytes(file, bytes).has_value());
        const auto restored = dk::read_file_bytes(file, size);
        REQUIRE(restored.has_value());
        REQUIRE(*restored == bytes);
        require_error(dk::read_file_bytes(file, size - 1), dk::ErrorCode::invalid_argument);
        require_error(dk::read_file_bytes(file, 0), dk::ErrorCode::invalid_argument);
        const auto large_limit = dk::read_file_bytes(file, std::numeric_limits<std::size_t>::max());
        REQUIRE(large_limit.has_value());
        REQUIRE(*large_limit == bytes);
    }
}

TEST_CASE("ordinary writes truncate existing files including empty replacement", "[io][file]")
{
    const TestDirectory directory;
    const auto file = directory.root / "replace.bin";
    REQUIRE(dk::write_file_bytes(file, patterned_bytes(150000)).has_value());
    const auto small = patterned_bytes(3);
    REQUIRE(dk::write_file_bytes(file, small).has_value());
    auto restored = dk::read_file_bytes(file);
    REQUIRE(restored.has_value());
    REQUIRE(*restored == small);
    REQUIRE(dk::write_file_bytes(file, {}).has_value());
    restored = dk::read_file_bytes(file);
    REQUIRE(restored.has_value());
    REQUIRE(restored->empty());
}

TEST_CASE("missing files and missing parent directories return not found with path context", "[io][file]")
{
    const TestDirectory directory;
    const auto missing = directory.root / "missing" / "data.bin";
    const auto read = dk::read_file_bytes(missing);
    require_error(read, dk::ErrorCode::not_found);
    REQUIRE(read.error().context.size() >= 3);
    REQUIRE(read.error().context[1] == "path: " + *dk::path_to_utf8(missing));
    const auto write = dk::write_file_bytes(missing, patterned_bytes(3));
    require_error(write, dk::ErrorCode::not_found);
    REQUIRE_FALSE(fs::exists(missing.parent_path()));
}

TEST_CASE("file IO rejects directory and invalid path arguments", "[io][file]")
{
    const TestDirectory directory;
    require_error(dk::read_file_bytes(directory.root), dk::ErrorCode::invalid_argument);
    require_error(dk::write_file_bytes(directory.root, {}), dk::ErrorCode::invalid_argument);
    require_error(dk::read_file_bytes({}), dk::ErrorCode::invalid_argument);
    require_error(dk::write_file_bytes({}, {}), dk::ErrorCode::invalid_argument);
    auto native = (directory.root / "visible.bin").native();
    native.push_back(fs::path::value_type{});
    native += fs::path{"hidden.bin"}.native();
    const fs::path invalid{native};
    require_error(dk::read_file_bytes(invalid), dk::ErrorCode::invalid_argument);
    require_error(dk::write_file_bytes(invalid, {}), dk::ErrorCode::invalid_argument);
    REQUIRE_FALSE(fs::exists(directory.root / "visible.bin"));
}

#ifdef _WIN32
TEST_CASE("Windows sharing failures return IO errors with system context", "[io][file]")
{
    const TestDirectory directory;
    const auto file = directory.root / "locked.bin";
    const auto original = patterned_bytes(17);
    REQUIRE(dk::write_file_bytes(file, original).has_value());
    {
        struct ExclusiveFile {
            HANDLE handle;
            ~ExclusiveFile() { if (handle != INVALID_HANDLE_VALUE) { CloseHandle(handle); } }
        } locked{CreateFileW(file.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
        REQUIRE(locked.handle != INVALID_HANDLE_VALUE);
        const auto read = dk::read_file_bytes(file);
        require_error(read, dk::ErrorCode::io_error);
        REQUIRE(read.error().context.size() >= 3);
        const auto write = dk::write_file_bytes(file, patterned_bytes(1));
        require_error(write, dk::ErrorCode::io_error);
        REQUIRE(write.error().context.size() >= 3);
    }
    const auto restored = dk::read_file_bytes(file);
    REQUIRE(restored.has_value());
    REQUIRE(*restored == original); // Opening failed before truncation in this sharing-error case.
}
#endif
