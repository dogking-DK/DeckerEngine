#include <dk/core/Error.hpp>
#include <magic_enum/magic_enum.hpp>

#include <utility>

namespace dk {

std::string_view error_code_name(ErrorCode code) noexcept
{
    const auto name = magic_enum::enum_name(code);
    return name.empty() ? "unknown" : name;
}

Error Error::with_context(std::string detail) const
{
    auto result = *this;
    result.context.push_back(std::move(detail));
    return result;
}

} // namespace dk
