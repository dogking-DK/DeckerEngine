#pragma once
#include <dk/core/Result.hpp>
#include <nlohmann/json.hpp>
#include <initializer_list>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace dk::detail {
using Json = nlohmann::json;
inline constexpr std::size_t json_limit = 16U * 1024U * 1024U;
struct FormatError { std::string message; ErrorCode code = ErrorCode::invalid_argument; };
inline void require(bool condition, std::string message, ErrorCode code = ErrorCode::invalid_argument)
{
    if (!condition) { throw FormatError{std::move(message), code}; }
}
inline Json parse_json(std::string_view text)
{
    require(text.size() <= json_limit, "JSON exceeds 16 MiB limit");
    std::vector<std::unordered_set<std::string>> keys;
    const auto callback = [&](int depth, Json::parse_event_t event, Json& value) {
        require(depth <= 64, "JSON nesting exceeds 64 levels");
        if (event == Json::parse_event_t::object_start) { keys.emplace_back(); }
        if (event == Json::parse_event_t::key) {
            require(keys.back().insert(value.get<std::string>()).second, "Duplicate JSON object key");
        }
        if (event == Json::parse_event_t::object_end) { keys.pop_back(); }
        return true;
    };
    return Json::parse(text.begin(), text.end(), callback);
}
inline void fields(const Json& value, std::initializer_list<std::string_view> names)
{
    require(value.is_object() && value.size() == names.size(), "Unexpected JSON object fields");
    for (const auto name : names) { require(value.contains(name), "Missing field: " + std::string{name}); }
}
inline void version_one(const Json& value)
{
    require(value.is_number_integer() && value == 1, "Unsupported format or component version", ErrorCode::not_supported);
}
inline std::string string_value(const Json& value)
{
    require(value.is_string(), "Expected JSON string");
    return value.get<std::string>();
}
template<typename Id> Id read_id(const Json& value)
{
    const auto result = Id::parse(string_value(value));
    require(result.has_value() && !result->is_nil(), "Invalid or nil persistent ID");
    return *result;
}
inline Error format_error(const FormatError& error, std::string operation)
{
    return Error{error.code, error.message, {std::move(operation)}};
}
inline Error json_error(const Json::exception& error, std::string operation)
{
    return Error{ErrorCode::invalid_argument, error.what(), {std::move(operation)}};
}
} // namespace dk::detail
