#include <dk/core/StableId.hpp>

#include <uuid.h>

#include <algorithm>
#include <exception>
#include <new>
#include <random>

namespace dk::detail {
namespace {

IdBytes copy_bytes(const uuids::uuid& id)
{
    const auto source = id.as_bytes();
    IdBytes bytes{};
    std::transform(source.begin(), source.end(), bytes.begin(), [](std::byte value) {
        return std::to_integer<std::uint8_t>(value);
    });
    return bytes;
}

} // namespace

Result<IdBytes> generate_id_bytes()
{
    try {
        // Each generator refers only to its own thread's random source.
        thread_local std::random_device random;
        thread_local uuids::basic_uuid_random_generator<std::random_device> generator{random};
        return copy_bytes(generator());
    } catch (const std::bad_alloc&) {
        throw;
    } catch (const std::exception& error) {
        return std::unexpected(Error{ErrorCode::internal_error,
            "UUID generation failed", {"stduuid", error.what()}});
    }
}

Result<IdBytes> parse_id_bytes(std::string_view text)
{
    if (text.size() != 36) {
        return std::unexpected(Error{ErrorCode::invalid_argument,
            "ID must contain exactly 36 characters"});
    }

    // stduuid also accepts compact/braced forms. Keep DeckerEngine's wire format strict.
    for (const std::size_t offset : {8U, 13U, 18U, 23U}) {
        if (text[offset] != '-') {
            return std::unexpected(Error{ErrorCode::invalid_argument,
                "ID separator must be a hyphen", {"offset " + std::to_string(offset)}});
        }
    }
    const auto id = uuids::uuid::from_string(text);
    if (!id) {
        return std::unexpected(Error{ErrorCode::invalid_argument,
            "ID contains a non-hexadecimal character"});
    }
    return copy_bytes(*id);
}

std::string format_id_bytes(const IdBytes& bytes)
{
    return uuids::to_string(uuids::uuid{bytes});
}

std::size_t hash_id_bytes(const IdBytes& bytes) noexcept
{
    return std::hash<uuids::uuid>{}(uuids::uuid{bytes});
}

} // namespace dk::detail
