#pragma once
#include <dk/assets/GltfImporter.hpp>
#include <nlohmann/json.hpp>
#include <cmath>

namespace dk::import_detail {
struct Failure { Error error; };
inline void require(bool value, std::string message, ErrorCode code = ErrorCode::invalid_argument)
{ if (!value) { throw Failure{Error{code, std::move(message)}}; } }
template<class T> T take(Result<T> value) { if (!value) { throw Failure{std::move(value.error())}; } return std::move(*value); }
inline String owned(std::string_view value) { return String{value.begin(), value.end()}; }
inline void consume(std::size_t& used, std::size_t count, std::size_t limit, std::string_view kind)
{ require(used <= limit && count <= limit - used, std::string{kind} + " budget exceeded"); used += count; }
inline std::size_t product(std::size_t count, std::size_t width, std::size_t limit, std::string_view kind)
{ require(width > 0 && count <= limit / width, std::string{kind} + " byte limit exceeded"); return count * width; }
inline void range(std::size_t offset, std::size_t size, std::size_t limit, std::string_view location)
{ require(offset <= limit && size <= limit - offset, "Out-of-range " + std::string{location}); }
} // namespace dk::import_detail
