#pragma once

#include <dk/io/Path.hpp>

#include <system_error>

namespace dk::detail {

[[nodiscard]] Result<std::string> checked_io_path(
    const std::filesystem::path& path, std::string_view operation);
[[nodiscard]] Error file_error(
    std::string_view operation, std::string_view path, std::error_code code);
[[nodiscard]] Error path_argument_error(
    std::string_view operation, std::string_view path, std::string_view message);

} // namespace dk::detail
