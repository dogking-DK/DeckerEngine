#include <dk/core/Error.hpp>

#include <utility>

namespace dk {

std::string_view error_code_name(ErrorCode code) noexcept
{
    switch (code) {
    case ErrorCode::invalid_argument: return "invalid_argument";
    case ErrorCode::invalid_state: return "invalid_state";
    case ErrorCode::not_found: return "not_found";
    case ErrorCode::io_error: return "io_error";
    case ErrorCode::not_supported: return "not_supported";
    case ErrorCode::internal_error: return "internal_error";
    case ErrorCode::conflict: return "conflict";
    }
    return "unknown";
}

Error Error::with_context(std::string detail) const
{
    auto result = *this;
    result.context.push_back(std::move(detail));
    return result;
}

} // namespace dk
