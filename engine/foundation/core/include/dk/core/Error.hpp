#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace dk {

enum class ErrorCode : std::uint32_t {
    invalid_argument = 1,
    invalid_state = 2,
    not_found = 3,
    io_error = 4,
    not_supported = 5,
    internal_error = 6,
};

[[nodiscard]] std::string_view error_code_name(ErrorCode code) noexcept;

struct Error {
    ErrorCode code;
    std::string message;
    // Ordered from the innermost operation to the outermost caller.
    std::vector<std::string> context{};

    [[nodiscard]] Error with_context(std::string detail) const;
};

} // namespace dk
