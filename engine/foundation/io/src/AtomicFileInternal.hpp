#pragma once

#include <dk/io/File.hpp>

#include <expected>
#include <memory>
#include <system_error>

namespace dk::detail {

// Private per-save backend. Also used by this module's fault-injection tests.
// create() acquires ownership only on success. close() consumes the handle even
// on failure and is a no-op thereafter. replace() must not perform copy/delete.
class AtomicWriteOps {
public:
    virtual ~AtomicWriteOps() = default;
    virtual Result<std::filesystem::path> next_path(const std::filesystem::path& parent) = 0;
    virtual std::error_code create(const std::filesystem::path& path) noexcept = 0;
    virtual std::expected<std::size_t, std::error_code> write(std::span<const std::byte> bytes) noexcept = 0;
    virtual std::error_code flush() noexcept = 0;
    virtual std::error_code close() noexcept = 0;
    virtual std::error_code replace(const std::filesystem::path& from,
        const std::filesystem::path& to) noexcept = 0;
    virtual std::error_code remove(const std::filesystem::path& path) noexcept = 0;
};

[[nodiscard]] std::unique_ptr<AtomicWriteOps> make_atomic_write_ops();
[[nodiscard]] Result<void> write_file_bytes_atomic_impl(
    const std::filesystem::path& path, std::span<const std::byte> bytes, AtomicWriteOps& ops);

} // namespace dk::detail
