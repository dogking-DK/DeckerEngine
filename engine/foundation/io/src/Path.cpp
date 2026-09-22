#include <dk/io/Path.hpp>

#include "IoInternal.hpp"

#include <cstdint>
#include <system_error>

namespace dk {
namespace {

bool valid_utf8(std::string_view text)
{
    for (std::size_t offset = 0; offset < text.size();) {
        const auto first = static_cast<unsigned char>(text[offset]);
        if (first <= 0x7fU) {
            ++offset;
            continue;
        }
        std::size_t count;
        std::uint32_t value;
        std::uint32_t minimum;
        if (first >= 0xc2U && first <= 0xdfU) {
            count = 2; value = first & 0x1fU; minimum = 0x80U;
        } else if (first >= 0xe0U && first <= 0xefU) {
            count = 3; value = first & 0x0fU; minimum = 0x800U;
        } else if (first >= 0xf0U && first <= 0xf4U) {
            count = 4; value = first & 0x07U; minimum = 0x10000U;
        } else {
            return false;
        }
        if (count > text.size() - offset) {
            return false;
        }
        for (std::size_t index = 1; index < count; ++index) {
            const auto next = static_cast<unsigned char>(text[offset + index]);
            if ((next & 0xc0U) != 0x80U) {
                return false;
            }
            value = (value << 6U) | (next & 0x3fU);
        }
        if (value < minimum || value > 0x10ffffU || (value >= 0xd800U && value <= 0xdfffU)) {
            return false;
        }
        offset += count;
    }
    return true;
}

std::unexpected<Error> invalid_encoding(std::string_view operation)
{
    return std::unexpected(Error{ErrorCode::invalid_argument,
        "Path must be nonempty valid Unicode without NUL", {std::string{operation}}});
}

} // namespace

Result<std::filesystem::path> path_from_utf8(std::string_view text)
{
    if (text.empty() || text.find('\0') != std::string_view::npos || !valid_utf8(text)) {
        return invalid_encoding("path_from_utf8");
    }
    try {
        return std::filesystem::path{std::u8string{text.begin(), text.end()}};
    } catch (const std::system_error&) {
        return invalid_encoding("path_from_utf8");
    }
}

Result<std::string> path_to_utf8(const std::filesystem::path& path)
{
    const auto& native = path.native();
    if (native.empty() || native.find(std::filesystem::path::value_type{}) != native.npos) {
        return invalid_encoding("path_to_utf8");
    }
    try {
        const auto encoded = path.generic_u8string();
        std::string text{reinterpret_cast<const char*>(encoded.data()), encoded.size()};
        if (!valid_utf8(text)) {
            return invalid_encoding("path_to_utf8");
        }
        return text;
    } catch (const std::system_error&) {
        return invalid_encoding("path_to_utf8");
    }
}

Result<ProjectPaths> ProjectPaths::create(const std::filesystem::path& root)
{
    const auto display = detail::checked_io_path(root, "ProjectPaths::create");
    if (!display) {
        return std::unexpected(display.error());
    }
    std::error_code code;
    const auto status = std::filesystem::status(root, code);
    if (code || !std::filesystem::exists(status)) {
        return std::unexpected(detail::file_error("ProjectPaths::create", *display,
            code ? code : std::make_error_code(std::errc::no_such_file_or_directory)));
    }
    if (!std::filesystem::is_directory(status)) {
        return std::unexpected(detail::path_argument_error(
            "ProjectPaths::create", *display, "Project root must be a directory"));
    }
    auto canonical = std::filesystem::canonical(root, code);
    if (code) {
        return std::unexpected(detail::file_error("ProjectPaths::create", *display, code));
    }
    return ProjectPaths{std::move(canonical)};
}

Result<std::filesystem::path> ProjectPaths::resolve(const std::filesystem::path& relative) const
{
    const auto display = detail::checked_io_path(relative, "ProjectPaths::resolve");
    if (!display) {
        return std::unexpected(display.error());
    }
    if (relative.has_root_name() || relative.has_root_directory()) {
        return std::unexpected(detail::path_argument_error(
            "ProjectPaths::resolve", *display, "Project path must be relative"));
    }
    const auto normalized = relative.lexically_normal();
    for (const auto& component : normalized) {
        if (component == "..") {
            return std::unexpected(detail::path_argument_error(
                "ProjectPaths::resolve", *display, "Project path escapes the root"));
        }
    }
    if (normalized == ".") {
        return root_;
    }
    return (root_ / normalized).lexically_normal();
}

namespace detail {

Result<std::string> checked_io_path(const std::filesystem::path& path, std::string_view operation)
{
    auto display = path_to_utf8(path);
    if (!display) {
        return std::unexpected(display.error().with_context(std::string{operation}));
    }
    return display;
}

Error file_error(std::string_view operation, std::string_view path, std::error_code code)
{
    Error error{code == std::errc::no_such_file_or_directory ? ErrorCode::not_found : ErrorCode::io_error,
        "Filesystem operation failed", {std::string{operation}, "path: " + std::string{path}}};
    if (code) {
        error.context.push_back(std::string{code.category().name()} + ":" + std::to_string(code.value())
            + " " + code.message());
    }
    return error;
}

Error path_argument_error(std::string_view operation, std::string_view path, std::string_view message)
{
    return Error{ErrorCode::invalid_argument, std::string{message},
        {std::string{operation}, "path: " + std::string{path}}};
}

} // namespace detail
} // namespace dk
