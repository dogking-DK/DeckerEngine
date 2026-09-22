#include <dk/core/Version.hpp>
#include <dk/core/VersionConfig.hpp>

namespace dk {

std::string_view version() noexcept
{
    return DK_VERSION_STRING;
}

} // namespace dk

