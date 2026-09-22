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

// Windows local-file save: exclusive sibling temp, flush/close, then rename-replace.
// Pre-commit failures preserve the target; cleanup failures include the temp path.
// Replaces file identity/metadata. Does not guarantee crash durability or isolation
// from concurrent path changes. Other platforms return not_supported.
[[nodiscard]] Result<void> write_file_bytes_atomic(
    const std::filesystem::path& path, std::span<const std::byte> bytes);

} // namespace dk
