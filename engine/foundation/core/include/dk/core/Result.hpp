#pragma once

#include <dk/core/Error.hpp>

#include <expected>

namespace dk {

template <typename T>
using Result = std::expected<T, Error>;

} // namespace dk
