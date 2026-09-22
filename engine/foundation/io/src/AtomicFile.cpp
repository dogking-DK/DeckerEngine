#include "AtomicFileInternal.hpp"
#include "IoInternal.hpp"

#include <dk/core/StableId.hpp>

#include <algorithm>
#include <utility>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#endif

namespace dk {
namespace {

namespace fs = std::filesystem;
constexpr std::string_view operation = "write_file_bytes_atomic";

Error unsupported(std::string_view path)
{
    return Error{ErrorCode::not_supported, "Atomic save requires a Windows local drive path",
        {std::string{operation}, "path: " + std::string{path}}};
}

#ifdef _WIN32
std::error_code windows_error() noexcept
{
    return {static_cast<int>(GetLastError()), std::system_category()};
}

bool reserved_name(std::wstring name)
{
    name = name.substr(0, name.find(L'.'));
    while (!name.empty() && name.back() == L' ') { name.pop_back(); }
    for (auto& character : name) {
        if (character >= L'a' && character <= L'z') {
            character = static_cast<wchar_t>(character - L'a' + L'A');
        }
    }
    if (name == L"CON" || name == L"PRN" || name == L"AUX" || name == L"NUL"
        || name == L"CONIN$" || name == L"CONOUT$") { return true; }
    if (name.size() != 4 || (name.substr(0, 3) != L"COM" && name.substr(0, 3) != L"LPT")) {
        return false;
    }
    return (name[3] >= L'0' && name[3] <= L'9') || name[3] == L'\u00b9'
        || name[3] == L'\u00b2' || name[3] == L'\u00b3';
}

Result<void> check_file_name(const fs::path& path, std::string_view display)
{
    const auto leaf = path.filename().native();
    if (leaf.empty() || leaf.back() == L' ' || leaf.back() == L'.'
        || leaf.find_first_of(L"<>:\"|?*") != std::wstring::npos || reserved_name(leaf)
        || std::any_of(leaf.begin(), leaf.end(), [](wchar_t c) { return c < 32; })) {
        return std::unexpected(detail::path_argument_error(operation, display, "Invalid ordinary file name"));
    }
    return {};
}

Result<void> check_windows_path(const fs::path& path, std::string_view display)
{
    const auto drive = path.root_name().native();
    if (drive.size() != 2 || drive[1] != L':'
        || GetDriveTypeW(path.root_path().c_str()) == DRIVE_REMOTE) {
        return std::unexpected(unsupported(display));
    }
    return check_file_name(path, display);
}

Result<void> check_target(const fs::path& path, std::string_view display)
{
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        const auto code = windows_error();
        if (code.value() == ERROR_FILE_NOT_FOUND) { return {}; }
        return std::unexpected(detail::file_error(operation, display, code));
    }
    if ((attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DEVICE)) != 0) {
        return std::unexpected(detail::path_argument_error(operation, display,
            "Atomic save target must be an ordinary file without a reparse point"));
    }
    return {};
}

class WindowsAtomicWriteOps final : public detail::AtomicWriteOps {
public:
    ~WindowsAtomicWriteOps() override { (void)close(); }

    Result<fs::path> next_path(const fs::path& parent) override
    {
        struct TemporaryFileTag;
        const auto id = StableId<TemporaryFileTag>::generate();
        if (!id) { return std::unexpected(id.error().with_context("atomic_save.temp_name")); }
        return parent / (".dk-save-" + id->to_string() + ".tmp");
    }

    std::error_code create(const fs::path& path) noexcept override
    {
        handle_ = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
            FILE_ATTRIBUTE_NORMAL, nullptr);
        return handle_ == INVALID_HANDLE_VALUE ? windows_error() : std::error_code{};
    }

    std::expected<std::size_t, std::error_code> write(std::span<const std::byte> bytes) noexcept override
    {
        DWORD written = 0;
        if (!WriteFile(handle_, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr)) {
            return std::unexpected(windows_error());
        }
        return written;
    }

    std::error_code flush() noexcept override
    {
        return FlushFileBuffers(handle_) ? std::error_code{} : windows_error();
    }

    std::error_code close() noexcept override
    {
        const HANDLE handle = std::exchange(handle_, INVALID_HANDLE_VALUE);
        if (handle == INVALID_HANDLE_VALUE) { return {}; }
        return CloseHandle(handle) ? std::error_code{} : windows_error();
    }

    std::error_code replace(const fs::path& from, const fs::path& to) noexcept override
    {
        return MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING)
            ? std::error_code{} : windows_error();
    }

    std::error_code remove(const fs::path& path) noexcept override
    {
        if (DeleteFileW(path.c_str())) { return {}; }
        const auto code = windows_error();
        return code.value() == ERROR_FILE_NOT_FOUND ? std::error_code{} : code;
    }

private:
    HANDLE handle_ = INVALID_HANDLE_VALUE;
};
#else
Result<void> check_file_name(const fs::path&, std::string_view display)
{
    return std::unexpected(unsupported(display));
}

Result<void> check_windows_path(const fs::path&, std::string_view display)
{
    return std::unexpected(unsupported(display));
}

Result<void> check_target(const fs::path&, std::string_view display)
{
    return std::unexpected(unsupported(display));
}
#endif

Result<fs::path> prepare_target(const fs::path& path)
{
    const auto display = detail::checked_io_path(path, operation);
    if (!display) { return std::unexpected(display.error()); }
    // Do not lexically normalize away a terminal slash, dot or dot-dot.
    if (path.filename().empty() || path.filename() == "." || path.filename() == "..") {
        return std::unexpected(detail::path_argument_error(operation, *display, "Target needs a file name"));
    }
    // Win32 absolute-path conversion can remove trailing dots/spaces. Validate
    // the original name first so an invalid spelling cannot alias another file.
    const auto name = check_file_name(path, *display);
    if (!name) { return std::unexpected(name.error()); }
    std::error_code code;
    const auto absolute = fs::absolute(path, code);
    if (code) { return std::unexpected(detail::file_error(operation, *display, code)); }
    auto checked = check_windows_path(absolute, *display);
    if (!checked) { return std::unexpected(checked.error()); }
    const auto parent = fs::canonical(absolute.parent_path(), code);
    if (code) { return std::unexpected(detail::file_error(operation, *display, code)); }
    if (!fs::is_directory(parent, code)) {
        if (code) { return std::unexpected(detail::file_error(operation, *display, code)); }
        return std::unexpected(detail::path_argument_error(operation, *display, "Parent must be a directory"));
    }
    auto target = parent / absolute.filename();
    checked = check_windows_path(target, *display);
    if (!checked) { return std::unexpected(checked.error()); }
    checked = check_target(target, *display);
    if (!checked) { return std::unexpected(checked.error()); }
    return target;
}

class TempGuard {
public:
    detail::AtomicWriteOps& ops;
    fs::path path;
    bool owned = false;

    ~TempGuard()
    {
        if (owned) {
            (void)ops.close();
            (void)ops.remove(path);
        }
    }

    Result<void> fail(Error error)
    {
        if (owned) {
            const auto close_error = ops.close();
            const auto remove_error = ops.remove(path);
            owned = false; // Explicit cleanup has run, including diagnostics on failure.
            const auto display = path_to_utf8(path);
            error.context.push_back("temporary: " + (display ? *display : "<invalid Unicode>"));
            const auto append = [&](std::string_view step, std::error_code code) {
                if (code) {
                    error.context.push_back(std::string{step} + ": " + code.category().name()
                        + ":" + std::to_string(code.value()) + " " + code.message());
                }
            };
            append("cleanup.close", close_error);
            append("cleanup.remove", remove_error);
        }
        return std::unexpected(std::move(error));
    }
};

} // namespace

namespace detail {

std::unique_ptr<AtomicWriteOps> make_atomic_write_ops()
{
#ifdef _WIN32
    return std::make_unique<WindowsAtomicWriteOps>();
#else
    return {};
#endif
}

Result<void> write_file_bytes_atomic_impl(const fs::path& path,
    std::span<const std::byte> bytes, AtomicWriteOps& ops)
{
    const auto target = prepare_target(path);
    if (!target) { return std::unexpected(target.error()); }
    const auto display = path_to_utf8(*target);
    if (!display) { return std::unexpected(display.error()); }
    TempGuard temp{ops, {}, false};
    for (int attempt = 0; attempt < 32; ++attempt) {
        auto candidate = ops.next_path(target->parent_path());
        if (!candidate) { return std::unexpected(candidate.error()); }
#ifdef _WIN32
        // The caller may save a name in our temporary-name namespace. Never
        // create that target before commit, even if it does not exist yet.
        const int comparison = CompareStringOrdinal(candidate->filename().c_str(), -1,
            target->filename().c_str(), -1, TRUE);
        if (comparison == 0) {
            return std::unexpected(file_error("atomic_save.compare_name", *display, windows_error()));
        }
        if (comparison == CSTR_EQUAL) {
            continue;
        }
#endif
        temp.path = std::move(*candidate);
        const auto code = ops.create(temp.path);
        if (!code) { temp.owned = true; break; }
        if (code != std::errc::file_exists) {
            auto error = file_error("atomic_save.create", *display, code);
            const auto name = path_to_utf8(temp.path);
            if (name) { error.context.push_back("candidate: " + *name); }
            return std::unexpected(std::move(error));
        }
    }
    if (!temp.owned) {
        return std::unexpected(file_error("atomic_save.create.exhausted", *display,
            std::make_error_code(std::errc::file_exists)));
    }
    for (std::size_t offset = 0; offset < bytes.size();) {
        const auto count = std::min(bytes.size() - offset, std::size_t{64U * 1024U});
        const auto written = ops.write(bytes.subspan(offset, count));
        if (!written || *written == 0 || *written > count) {
            return temp.fail(file_error("atomic_save.write", *display,
                written ? std::make_error_code(std::errc::io_error) : written.error()));
        }
        offset += *written;
    }
    if (const auto code = ops.flush()) {
        return temp.fail(file_error("atomic_save.flush", *display, code));
    }
    if (const auto code = ops.close()) {
        return temp.fail(file_error("atomic_save.close", *display, code));
    }
    if (const auto checked = check_target(*target, *display); !checked) {
        return temp.fail(checked.error());
    }
    if (const auto code = ops.replace(temp.path, *target)) {
        return temp.fail(file_error("atomic_save.replace", *display, code));
    }
    temp.owned = false; // Commit point: no fallible operations after successful rename.
    return {};
}

} // namespace detail

Result<void> write_file_bytes_atomic(const fs::path& path, std::span<const std::byte> bytes)
{
    auto ops = detail::make_atomic_write_ops();
    if (!ops) { return std::unexpected(unsupported("<platform>")); }
    return detail::write_file_bytes_atomic_impl(path, bytes, *ops);
}

} // namespace dk
