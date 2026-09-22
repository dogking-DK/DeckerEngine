#include <dk/io/File.hpp>

#include "IoInternal.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <fstream>
#include <system_error>

namespace dk {
namespace {

Result<void> check_file(const std::filesystem::path& path, std::string_view display,
    std::string_view operation, bool allow_missing)
{
    std::error_code code;
    const auto status = std::filesystem::status(path, code);
    if (code) {
        if (allow_missing && code == std::errc::no_such_file_or_directory) {
            return {};
        }
        return std::unexpected(detail::file_error(operation, display, code));
    }
    if (!std::filesystem::exists(status)) {
        if (allow_missing) {
            return {};
        }
        return std::unexpected(detail::file_error(operation, display,
            std::make_error_code(std::errc::no_such_file_or_directory)));
    }
    if (!std::filesystem::is_regular_file(status)) {
        return std::unexpected(detail::path_argument_error(operation, display, "Path must be a regular file"));
    }
    return {};
}

std::unexpected<Error> stream_error(std::string_view operation, std::string_view path, int code)
{
    return std::unexpected(detail::file_error(operation, path,
        code != 0 ? std::error_code{code, std::generic_category()}
                  : std::make_error_code(std::errc::io_error)));
}

} // namespace

Result<ByteBuffer> read_file_bytes(const std::filesystem::path& path, std::size_t max_bytes)
{
    const auto display = detail::checked_io_path(path, "read_file_bytes");
    if (!display) {
        return std::unexpected(display.error());
    }
    const auto checked = check_file(path, *display, "read_file_bytes", false);
    if (!checked) {
        return std::unexpected(checked.error());
    }
    std::ifstream input;
    errno = 0;
    input.open(path, std::ios::binary);
    if (!input.is_open()) {
        return stream_error("read_file_bytes.open", *display, errno);
    }

    ByteBuffer result;
    const auto limit = std::min(max_bytes, result.max_size());
    std::array<char, 64U * 1024U> buffer;
    for (;;) {
        const auto remaining = limit - result.size();
        const auto requested = remaining == 0 ? std::size_t{1} : std::min(remaining, buffer.size());
        errno = 0;
        input.read(buffer.data(), static_cast<std::streamsize>(requested));
        const int read_error = errno;
        const auto amount = static_cast<std::size_t>(input.gcount());
        if (input.bad() || (input.fail() && !input.eof())) {
            return stream_error("read_file_bytes.read", *display, read_error);
        }
        if (amount > remaining) {
            return std::unexpected(detail::path_argument_error(
                "read_file_bytes", *display, "File exceeds the read size limit"));
        }
        const auto bytes = std::as_bytes(std::span{buffer.data(), amount});
        result.insert(result.end(), bytes.begin(), bytes.end());
        if (input.eof()) {
            return result;
        }
    }
}

Result<void> write_file_bytes(const std::filesystem::path& path, std::span<const std::byte> bytes)
{
    const auto display = detail::checked_io_path(path, "write_file_bytes");
    if (!display) {
        return std::unexpected(display.error());
    }
    const auto checked = check_file(path, *display, "write_file_bytes", true);
    if (!checked) {
        return std::unexpected(checked.error());
    }
    std::ofstream output;
    errno = 0;
    output.open(path, std::ios::binary | std::ios::trunc);
    if (!output.is_open()) {
        return stream_error("write_file_bytes.open", *display, errno);
    }
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const auto count = std::min(bytes.size() - offset, std::size_t{64U * 1024U});
        errno = 0;
        output.write(reinterpret_cast<const char*>(bytes.data() + offset), static_cast<std::streamsize>(count));
        if (!output) {
            return stream_error("write_file_bytes.write", *display, errno);
        }
        offset += count;
    }
    errno = 0;
    output.flush();
    if (!output) {
        return stream_error("write_file_bytes.flush", *display, errno);
    }
    errno = 0;
    output.close();
    if (!output) {
        return stream_error("write_file_bytes.close", *display, errno);
    }
    return {};
}

} // namespace dk
