#pragma once
#include <dk/memory/Allocator.hpp>
#include <string>
#include <vector>

namespace dk {
template<class T> using Vector = std::vector<T, memory::Allocator<T>>;
using String = std::basic_string<char, std::char_traits<char>, memory::Allocator<char>>;
} // namespace dk
