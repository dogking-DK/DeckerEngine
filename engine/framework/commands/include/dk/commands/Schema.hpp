#pragma once
#include <dk/core/Result.hpp>
#include <nlohmann/json.hpp>
#include <string_view>

namespace dk {
using Json = nlohmann::json;
namespace schema {
[[nodiscard]] Json object(Json properties = Json::object(), Json required = Json::array());
[[nodiscard]] Json array(Json items, std::size_t minimum = 0, std::size_t maximum = 10000);
[[nodiscard]] Json string(std::size_t minimum = 0, std::size_t maximum = 1024);
[[nodiscard]] Json integer();
[[nodiscard]] Json number();
[[nodiscard]] Json boolean();
[[nodiscard]] Json nullable(Json value);
[[nodiscard]] Result<void> check(const Json& definition);
[[nodiscard]] Result<void> validate(const Json& definition, const Json& value);
} // namespace schema
[[nodiscard]] Result<void> validate_command_value(const Json& value, std::size_t byte_limit);
[[nodiscard]] Result<Json> parse_command_json(std::string_view text);
} // namespace dk
