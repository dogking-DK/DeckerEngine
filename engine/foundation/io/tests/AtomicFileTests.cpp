#include <dk/core/StableId.hpp>
#include <dk/io/File.hpp>
#include <dk/io/Path.hpp>

#include "../src/AtomicFileInternal.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <set>
#include <string>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

namespace {

namespace fs = std::filesystem;

struct SaveDirectory {
    fs::path root;
    SaveDirectory()
    {
        const auto id = dk::AssetId::generate();
        REQUIRE(id.has_value());
        root = fs::current_path() / "test-artifacts" / DK_TEST_CONFIG / "atomic-io" / id->to_string();
        REQUIRE(fs::create_directories(root));
    }
};

dk::ByteBuffer bytes(std::size_t size, unsigned int seed = 0)
{
    dk::ByteBuffer data(size);
    for (std::size_t index = 0; index < size; ++index) {
        data[index] = static_cast<std::byte>((index + seed) % 256U);
    }
    return data;
}

dk::ByteBuffer read(const fs::path& path)
{
    const auto result = dk::read_file_bytes(path);
    REQUIRE(result.has_value());
    return *result;
}

std::set<fs::path> entries(const fs::path& directory)
{
    std::set<fs::path> result;
    for (const auto& entry : fs::directory_iterator(directory)) { result.insert(entry.path().filename()); }
    return result;
}

void require_error(const dk::Result<void>& result, dk::ErrorCode code)
{
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().code == code);
    REQUIRE_FALSE(result.error().message.empty());
    REQUIRE_FALSE(result.error().context.empty());
}

bool has_context(const dk::Error& error, std::string_view text)
{
    return std::any_of(error.context.begin(), error.context.end(), [&](const auto& item) {
        return item.find(text) != std::string::npos;
    });
}

enum class Fault { none, create, write, zero_write, flush, close, replace };

// Wraps the production Win32 backend: real exclusive files, writes and renames.
// Faults are local to one save; no process-wide switches or user files involved.
class FaultOps final : public dk::detail::AtomicWriteOps {
public:
    std::unique_ptr<dk::detail::AtomicWriteOps> native = dk::detail::make_atomic_write_ops();
    Fault fault = Fault::none;
    fs::path collision;
    fs::path last_temp;
    bool collide_forever = false;
    bool fail_cleanup = false;
    bool fail_cleanup_close = false;
    bool short_writes = false;
    int candidates = 0;
    int replace_calls = 0;
    std::size_t written_bytes = 0;

    dk::Result<fs::path> next_path(const fs::path& parent) override
    {
        ++candidates;
        if (!collision.empty() && (collide_forever || candidates == 1)) {
            last_temp = collision;
        } else {
            const auto result = native->next_path(parent);
            if (!result) { return std::unexpected(result.error()); }
            last_temp = *result;
        }
        return last_temp;
    }

    std::error_code create(const fs::path& path) noexcept override
    {
        if (fault == Fault::create) { return std::make_error_code(std::errc::permission_denied); }
        return native->create(path);
    }

    std::expected<std::size_t, std::error_code> write(std::span<const std::byte> data) noexcept override
    {
        if (fault == Fault::write && written_bytes > 0) {
            return std::unexpected(std::make_error_code(std::errc::no_space_on_device));
        }
        if (fault == Fault::zero_write) { return std::size_t{0}; }
        if (short_writes || fault == Fault::write) { data = data.first(std::min(data.size(), std::size_t{4096})); }
        const auto result = native->write(data);
        if (result) { written_bytes += *result; }
        return result;
    }

    std::error_code flush() noexcept override
    {
        const auto result = native->flush();
        if (result) { return result; }
        return fault == Fault::flush ? std::make_error_code(std::errc::io_error) : std::error_code{};
    }

    std::error_code close() noexcept override
    {
        const auto result = native->close();
        if (result) { return result; }
        if (fault == Fault::close || fail_cleanup_close) {
            fault = Fault::none;
            fail_cleanup_close = false;
            return std::make_error_code(std::errc::io_error);
        }
        return {};
    }

    std::error_code replace(const fs::path& from, const fs::path& to) noexcept override
    {
        ++replace_calls;
        if (fault == Fault::replace) { return std::make_error_code(std::errc::permission_denied); }
        return native->replace(from, to);
    }

    std::error_code remove(const fs::path& path) noexcept override
    {
        if (fail_cleanup) { return std::make_error_code(std::errc::permission_denied); }
        return native->remove(path);
    }
};

} // namespace

TEST_CASE("atomic save creates and replaces Unicode binary files without temp residue", "[io][atomic]")
{
    const SaveDirectory directory;
    const auto name = dk::path_from_utf8("场景 📦.bin");
    REQUIRE(name.has_value());
    const auto target = directory.root / *name;
    for (const auto size : {std::size_t{150000}, std::size_t{3}, std::size_t{0}, std::size_t{256}}) {
        const auto data = bytes(size);
        REQUIRE(dk::write_file_bytes_atomic(target, data).has_value());
        REQUIRE(read(target) == data);
        REQUIRE(entries(directory.root) == std::set<fs::path>{*name});
    }
}

TEST_CASE("atomic save accepts relative targets and captures an existing parent", "[io][atomic]")
{
    const SaveDirectory directory;
    const auto target = directory.root / "relative.bin";
    const auto relative = fs::relative(target, fs::current_path());
    REQUIRE_FALSE(relative.is_absolute());
    const auto data = bytes(7);
    REQUIRE(dk::write_file_bytes_atomic(relative, data).has_value());
    REQUIRE(read(target) == data);
    REQUIRE(entries(directory.root).size() == 1);
}

TEST_CASE("atomic save rejects invalid names directories and missing parents before writing", "[io][atomic]")
{
    const SaveDirectory directory;
    const auto data = bytes(5);
    require_error(dk::write_file_bytes_atomic({}, data), dk::ErrorCode::invalid_argument);
    require_error(dk::write_file_bytes_atomic(directory.root, data), dk::ErrorCode::invalid_argument);
    require_error(dk::write_file_bytes_atomic(directory.root / ".", data), dk::ErrorCode::invalid_argument);
    require_error(dk::write_file_bytes_atomic(directory.root / "..", data), dk::ErrorCode::invalid_argument);
    require_error(dk::write_file_bytes_atomic(directory.root / "", data), dk::ErrorCode::invalid_argument);
    require_error(dk::write_file_bytes_atomic(directory.root / "missing" / "data.bin", data), dk::ErrorCode::not_found);
    for (const auto* name : {L"file:stream", L"tail.", L"tail ", L"NUL.bin", L"con", L"COM1", L"LPT\u00b2.txt", L"a?b"}) {
        const auto display = dk::path_to_utf8(fs::path{name});
        REQUIRE(display.has_value());
        CAPTURE(*display);
        require_error(dk::write_file_bytes_atomic(directory.root / name, data), dk::ErrorCode::invalid_argument);
    }
    auto invalid = (directory.root / "visible.bin").native();
    invalid.push_back(L'\0');
    invalid += L"hidden";
    require_error(dk::write_file_bytes_atomic(fs::path{invalid}, data), dk::ErrorCode::invalid_argument);
    require_error(dk::write_file_bytes_atomic(L"\\\\server\\share\\target.bin", data), dk::ErrorCode::not_supported);
    require_error(dk::write_file_bytes_atomic(L"\\\\?\\C:\\target.bin", data), dk::ErrorCode::not_supported);
    REQUIRE(entries(directory.root).empty());
}

TEST_CASE("atomic save preserves old or absent target at every precommit failure", "[io][atomic]")
{
    const auto original = bytes(257, 3);
    const auto replacement = bytes(150000, 77);
    for (const bool exists : {false, true}) {
        for (const auto fault : {Fault::create, Fault::write, Fault::zero_write, Fault::flush, Fault::close, Fault::replace}) {
            CAPTURE(exists, static_cast<int>(fault));
            const SaveDirectory directory;
            const auto target = directory.root / "state.bin";
            if (exists) { REQUIRE(dk::write_file_bytes(target, original).has_value()); }
            FaultOps ops;
            ops.fault = fault;
            const auto result = dk::detail::write_file_bytes_atomic_impl(target, replacement, ops);
            require_error(result, dk::ErrorCode::io_error);
            REQUIRE(has_context(result.error(), "atomic_save."));
            if (fault == Fault::write) { REQUIRE(ops.written_bytes == 4096); }
            if (fault != Fault::replace) { REQUIRE(ops.replace_calls == 0); }
            if (exists) {
                REQUIRE(read(target) == original);
                REQUIRE(entries(directory.root) == std::set<fs::path>{"state.bin"});
            } else {
                REQUIRE_FALSE(fs::exists(target));
                REQUIRE(entries(directory.root).empty());
            }
        }
    }
}

TEST_CASE("atomic save handles short writes until the complete replacement is ready", "[io][atomic]")
{
    const SaveDirectory directory;
    const auto target = directory.root / "short.bin";
    const auto replacement = bytes(150003, 13);
    FaultOps ops;
    ops.short_writes = true;
    REQUIRE(dk::detail::write_file_bytes_atomic_impl(target, replacement, ops).has_value());
    REQUIRE(ops.written_bytes == replacement.size());
    REQUIRE(ops.replace_calls == 1);
    REQUIRE(ops.last_temp.parent_path() == fs::canonical(directory.root));
    REQUIRE(read(target) == replacement);
    REQUIRE(entries(directory.root) == std::set<fs::path>{"short.bin"});
}

TEST_CASE("atomic temp collisions never overwrite or clean another saves file", "[io][atomic]")
{
    const SaveDirectory directory;
    const auto sentinel = directory.root / ".dk-save-existing.tmp";
    const auto sentinel_data = bytes(9, 111);
    REQUIRE(dk::write_file_bytes(sentinel, sentinel_data).has_value());
    const auto target = directory.root / "saved.bin";
    const auto data = bytes(200);
    FaultOps ops;
    ops.collision = sentinel;
    REQUIRE(dk::detail::write_file_bytes_atomic_impl(target, data, ops).has_value());
    REQUIRE(ops.candidates == 2);
    REQUIRE(read(sentinel) == sentinel_data);
    REQUIRE(read(target) == data);
    REQUIRE(entries(directory.root).size() == 2);
}

TEST_CASE("atomic temp candidates cannot alias an absent target name", "[io][atomic]")
{
    const SaveDirectory directory;
    const auto target = directory.root / ".DK-SAVE-target.TMP";
    FaultOps ops;
    ops.collision = directory.root / ".dk-save-TARGET.tmp";
    const auto data = bytes(93, 1);
    REQUIRE(dk::detail::write_file_bytes_atomic_impl(target, data, ops).has_value());
    REQUIRE(ops.candidates == 2);
    REQUIRE(read(target) == data);
    REQUIRE(entries(directory.root).size() == 1);
}

TEST_CASE("atomic temp collision exhaustion keeps all existing files", "[io][atomic]")
{
    const SaveDirectory directory;
    const auto sentinel = directory.root / ".dk-save-existing.tmp";
    const auto target = directory.root / "state.bin";
    const auto original = bytes(25);
    REQUIRE(dk::write_file_bytes(sentinel, original).has_value());
    REQUIRE(dk::write_file_bytes(target, original).has_value());
    FaultOps ops;
    ops.collision = sentinel;
    ops.collide_forever = true;
    const auto result = dk::detail::write_file_bytes_atomic_impl(target, bytes(19, 3), ops);
    require_error(result, dk::ErrorCode::io_error);
    REQUIRE(has_context(result.error(), "exhausted"));
    REQUIRE(ops.candidates == 32);
    REQUIRE(ops.replace_calls == 0);
    REQUIRE(read(sentinel) == original);
    REQUIRE(read(target) == original);
    REQUIRE(entries(directory.root).size() == 2);
}

TEST_CASE("atomic cleanup failures retain primary error and report the residual path", "[io][atomic]")
{
    const SaveDirectory directory;
    const auto target = directory.root / "state.bin";
    const auto original = bytes(35);
    REQUIRE(dk::write_file_bytes(target, original).has_value());
    FaultOps ops;
    ops.fault = Fault::write;
    ops.fail_cleanup = true;
    ops.fail_cleanup_close = true;
    const auto result = dk::detail::write_file_bytes_atomic_impl(target, bytes(9000, 7), ops);
    require_error(result, dk::ErrorCode::io_error);
    REQUIRE(has_context(result.error(), "atomic_save.write"));
    REQUIRE(has_context(result.error(), "cleanup.close"));
    REQUIRE(has_context(result.error(), "cleanup.remove"));
    const auto temp_text = dk::path_to_utf8(ops.last_temp);
    REQUIRE(temp_text.has_value());
    REQUIRE(has_context(result.error(), *temp_text));
    REQUIRE(read(target) == original);
    REQUIRE(read(ops.last_temp).size() == 4096);
    REQUIRE(fs::remove(ops.last_temp)); // Only this test-owned residual file.
    REQUIRE(entries(directory.root) == std::set<fs::path>{"state.bin"});
}

TEST_CASE("Windows replacement sharing failure preserves old data and cleans temporary file", "[io][atomic]")
{
    const SaveDirectory directory;
    const auto target = directory.root / "locked.bin";
    const auto original = bytes(1000);
    REQUIRE(dk::write_file_bytes(target, original).has_value());
    {
        struct LockedFile {
            HANDLE handle;
            ~LockedFile() { if (handle != INVALID_HANDLE_VALUE) { CloseHandle(handle); } }
        } lock{CreateFileW(target.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
        REQUIRE(lock.handle != INVALID_HANDLE_VALUE);
        const auto result = dk::write_file_bytes_atomic(target, bytes(150000, 91));
        require_error(result, dk::ErrorCode::io_error);
        REQUIRE(has_context(result.error(), "atomic_save.replace"));
        REQUIRE(has_context(result.error(), "system:"));
        REQUIRE(read(target) == original);
        REQUIRE(entries(directory.root) == std::set<fs::path>{"locked.bin"});
    }
    const auto replacement = bytes(13);
    REQUIRE(dk::write_file_bytes_atomic(target, replacement).has_value());
    REQUIRE(read(target) == replacement);
}

TEST_CASE("Windows readonly destination is preserved on failed atomic replacement", "[io][atomic]")
{
    const SaveDirectory directory;
    const auto target = directory.root / "readonly.bin";
    const auto original = bytes(10);
    REQUIRE(dk::write_file_bytes(target, original).has_value());
    struct RestoreAttributes {
        fs::path path;
        ~RestoreAttributes() { SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL); }
    } restore{target};
    REQUIRE(SetFileAttributesW(target.c_str(), FILE_ATTRIBUTE_READONLY));
    const auto result = dk::write_file_bytes_atomic(target, bytes(9000));
    require_error(result, dk::ErrorCode::io_error);
    REQUIRE(has_context(result.error(), "atomic_save.replace"));
    REQUIRE(read(target) == original);
    REQUIRE(entries(directory.root) == std::set<fs::path>{"readonly.bin"});
}

TEST_CASE("atomic replacement changes file identity without changing other hard links", "[io][atomic]")
{
    const SaveDirectory directory;
    const auto target = directory.root / "state.bin";
    const auto alias = directory.root / "alias.bin";
    const auto original = bytes(51);
    const auto replacement = bytes(73, 123);
    REQUIRE(dk::write_file_bytes(target, original).has_value());
    fs::create_hard_link(target, alias);
    REQUIRE(dk::write_file_bytes_atomic(target, replacement).has_value());
    REQUIRE(read(alias) == original);
    REQUIRE(read(target) == replacement);
    REQUIRE_FALSE(fs::equivalent(target, alias));
    REQUIRE(entries(directory.root).size() == 2);
}

TEST_CASE("atomic save rejects a target symbolic link without modifying its referent", "[io][atomic]")
{
    const SaveDirectory directory;
    const auto original = directory.root / "original.bin";
    const auto link = directory.root / "link.bin";
    const auto data = bytes(15);
    REQUIRE(dk::write_file_bytes(original, data).has_value());
    std::error_code code;
    fs::create_symlink(original, link, code);
    if (code == std::errc::permission_denied || code.value() == ERROR_PRIVILEGE_NOT_HELD) {
        SKIP("Creating a symlink requires developer mode or symlink privilege");
    }
    REQUIRE_FALSE(code);
    require_error(dk::write_file_bytes_atomic(link, bytes(700)), dk::ErrorCode::invalid_argument);
    REQUIRE(fs::is_symlink(link));
    REQUIRE(read(original) == data);
    REQUIRE(entries(directory.root).size() == 2);
}

#else
TEST_CASE("atomic save reports unsupported platforms without touching a file", "[io][atomic]")
{
    const auto result = dk::write_file_bytes_atomic("unused.bin", {});
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().code == dk::ErrorCode::not_supported);
}
#endif
