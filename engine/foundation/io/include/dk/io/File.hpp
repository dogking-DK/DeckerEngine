#pragma once

#include <dk/core/Result.hpp>

#include <cstddef>
#include <filesystem>
#include <span>
#include <vector>

namespace dk {

using ByteBuffer = std::vector<std::byte>;
inline constexpr std::size_t default_file_read_limit = 64U * 1024U * 1024U;

[[nodiscard]] Result<ByteBuffer> read_file_bytes(
    const std::filesystem::path& path, std::size_t max_bytes = default_file_read_limit);

// Creates or truncates. Failure may leave partial contents; this is not atomic save.
// Does not create parent directories or guarantee durable storage.
[[nodiscard]] Result<void> write_file_bytes(
    const std::filesystem::path& path, std::span<const std::byte> bytes);

} // namespace dk
