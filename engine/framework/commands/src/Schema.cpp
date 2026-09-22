#include <dk/commands/Schema.hpp>
#include <algorithm>
#include <cmath>
#include <set>

namespace dk {
namespace {
Result<void> invalid(std::string message) { return std::unexpected(Error{ErrorCode::invalid_argument, std::move(message)}); }
bool size_value(const Json& value) { return value.is_number_unsigned() || (value.is_number_integer() && value.get<std::int64_t>() >= 0); }
int numeric_compare(const Json& a, const Json& b) {
    if (a.is_number_integer() && b.is_number_integer()) {
        const bool an = !a.is_number_unsigned() && a.get<std::int64_t>() < 0;
        const bool bn = !b.is_number_unsigned() && b.get<std::int64_t>() < 0;
        if (an != bn) return an ? -1 : 1;
        if (an) { const auto av = a.get<std::int64_t>(), bv = b.get<std::int64_t>(); return av < bv ? -1 : (av > bv ? 1 : 0); }
        const auto av = a.get<std::uint64_t>(), bv = b.get<std::uint64_t>(); return av < bv ? -1 : (av > bv ? 1 : 0);
    }
    if (a.is_number_float() && b.is_number_integer()) return -numeric_compare(b, a);
    if (a.is_number_integer()) {
        const double d = b.get<double>();
        Json part;
        if (a.is_number_unsigned()) {
            if (d < 0) return 1;
            if (d >= 0x1p64) return -1;
            part = static_cast<std::uint64_t>(d);
        } else {
            if (d < -0x1p63) return 1;
            if (d >= 0x1p63) return -1;
            part = static_cast<std::int64_t>(d);
        }
        const auto compared = numeric_compare(a, part);
        if (compared != 0) return compared;
        const auto truncated = part.get<double>();
        return truncated < d ? -1 : (truncated > d ? 1 : 0);
    }
    const double av = a.get<double>(), bv = b.get<double>(); return av < bv ? -1 : (av > bv ? 1 : 0);
}
bool equivalent(const Json& a, const Json& b) {
    if (a.is_number() && b.is_number()) return numeric_compare(a, b) == 0;
    if (a.type() != b.type() || a.size() != b.size()) return false;
    if (a.is_array()) { for (std::size_t i = 0; i < a.size(); ++i) if (!equivalent(a[i], b[i])) return false; return true; }
    if (a.is_object()) { for (const auto& [key, value] : a.items()) if (!b.contains(key) || !equivalent(value, b[key])) return false; return true; }
    return a == b;
}
Result<void> inspect(const Json& value, std::size_t depth, std::size_t& nodes) {
    if (depth > 64 || ++nodes > 200000) return invalid("JSON structural limit exceeded");
    if (value.is_discarded() || value.is_binary()) return invalid("Unsupported JSON value");
    if (value.is_number_float() && !std::isfinite(value.get<double>())) return invalid("Non-finite JSON number");
    if (value.is_structured()) for (const auto& child : value) {
        auto result = inspect(child, depth + 1, nodes);
        if (!result) return result;
    }
    return {};
}
bool matches(std::string_view type, const Json& value) {
    if (type == "object") return value.is_object();
    if (type == "array") return value.is_array();
    if (type == "string") return value.is_string();
    if (type == "integer") return value.is_number_integer();
    if (type == "number") return value.is_number();
    if (type == "boolean") return value.is_boolean();
    return type == "null" && value.is_null();
}
Result<void> check_schema(const Json& s, std::size_t depth) {
    if (depth > 64 || !s.is_object()) return invalid("Schema must be an object within depth limit");
    static const std::set<std::string> keywords{"type", "properties", "required", "additionalProperties", "items", "minItems", "maxItems", "minLength", "maxLength", "minimum", "maximum", "enum", "x-dk-integer-token"};
    static const std::set<std::string> types{"object", "array", "string", "integer", "number", "boolean", "null"};
    for (const auto& [key, v] : s.items()) {
        if (!keywords.contains(key)) return invalid("Unknown schema keyword: " + key);
        if (key == "type") {
            const Json values = v.is_array() ? v : Json::array({v});
            std::set<std::string> seen;
            if (values.empty()) return invalid("Empty type list");
            for (const auto& t : values) if (!t.is_string() || !types.contains(t.get<std::string>()) || !seen.insert(t.get<std::string>()).second) return invalid("Invalid schema type");
            if (seen.contains("integer") && (!s.contains("x-dk-integer-token") || s["x-dk-integer-token"] != true)) return invalid("Integer schema requires x-dk-integer-token");
        } else if (key == "properties") {
            if (!v.is_object()) return invalid("properties must be an object");
            for (const auto& child : v) { auto r = check_schema(child, depth + 1); if (!r) return r; }
        } else if (key == "items") {
            auto r = check_schema(v, depth + 1); if (!r) return r;
        } else if (key == "required") {
            if (!v.is_array()) return invalid("required must be an array");
            std::set<std::string> seen;
            for (const auto& name : v) if (!name.is_string() || !s.contains("properties") || !s["properties"].contains(name.get<std::string>()) || !seen.insert(name.get<std::string>()).second) return invalid("Invalid required property");
        } else if (key == "additionalProperties") {
            if (!v.is_boolean()) return invalid("additionalProperties must be boolean");
        } else if (key == "x-dk-integer-token") {
            if (v != true) return invalid("x-dk-integer-token must be true");
        } else if (key == "enum") {
            if (!v.is_array() || v.empty() || v.size() > 256) return invalid("enum requires 1-256 values");
            for (std::size_t i = 0; i < v.size(); ++i) for (std::size_t j = 0; j < i; ++j) if (equivalent(v[i], v[j])) return invalid("Duplicate enum value");
        } else if (key == "minimum" || key == "maximum") {
            if (!v.is_number()) return invalid("Numeric bound required");
        } else if (!size_value(v)) return invalid("Nonnegative integer bound required");
    }
    for (const auto& bounds : {std::pair{"minItems", "maxItems"}, std::pair{"minLength", "maxLength"}, std::pair{"minimum", "maximum"}})
        if (s.contains(bounds.first) && s.contains(bounds.second) && numeric_compare(s[bounds.first], s[bounds.second]) > 0) return invalid("Inverted schema bounds");
    return {};
}
Result<void> validate_value(const Json& s, const Json& v, const std::string& path) {
    auto fail = [&path](std::string message) { return invalid(path + ": " + message); };
    if (s.contains("type")) {
        const auto& t = s["type"];
        bool match = false;
        if (t.is_array()) { for (const auto& type : t) match = match || matches(type.get<std::string>(), v); }
        else match = matches(t.get<std::string>(), v);
        if (!match) return fail("Unexpected value type");
    }
    if (s.contains("enum") && std::none_of(s["enum"].begin(), s["enum"].end(), [&v](const Json& item) { return equivalent(item, v); })) return fail("Value outside enum");
    if (v.is_object()) {
        if (s.contains("required")) for (const auto& key : s["required"]) if (!v.contains(key.get<std::string>())) return fail("Missing " + key.get<std::string>());
        for (const auto& [key, child] : v.items()) {
            if (s.contains("properties") && s["properties"].contains(key)) { auto r = validate_value(s["properties"][key], child, path + "/" + key); if (!r) return r; }
            else if (s.contains("additionalProperties") && s["additionalProperties"] == false) return fail("Unknown property " + key);
        }
    }
    if (v.is_array()) {
        if ((s.contains("minItems") && v.size() < s["minItems"].get<std::size_t>()) || (s.contains("maxItems") && v.size() > s["maxItems"].get<std::size_t>())) return fail("Array length outside bounds");
        if (s.contains("items")) for (std::size_t i = 0; i < v.size(); ++i) { auto r = validate_value(s["items"], v[i], path + "/" + std::to_string(i)); if (!r) return r; }
    }
    if (v.is_string()) {
        std::size_t length = 0;
        for (const unsigned char c : v.get_ref<const std::string&>()) if ((c & 0xc0) != 0x80) ++length;
        if ((s.contains("minLength") && length < s["minLength"].get<std::size_t>()) || (s.contains("maxLength") && length > s["maxLength"].get<std::size_t>())) return fail("String length outside bounds");
    }
    if (v.is_number() && ((s.contains("minimum") && numeric_compare(v, s["minimum"]) < 0) || (s.contains("maximum") && numeric_compare(v, s["maximum"]) > 0))) return fail("Number outside bounds");
    return {};
}
} // namespace
Result<void> validate_command_value(const Json& value, std::size_t byte_limit) {
    std::size_t nodes = 0;
    auto r = inspect(value, 0, nodes); if (!r) return r;
    try { if (value.dump().size() > byte_limit) return invalid("JSON byte limit exceeded"); }
    catch (const Json::exception&) { return invalid("Invalid UTF-8 JSON value"); }
    return {};
}
Result<Json> parse_command_json(std::string_view text) {
    if (text.size() > 1024 * 1024) return std::unexpected(Error{ErrorCode::invalid_argument, "JSON input exceeds 1 MiB"});
    try {
        std::vector<std::set<std::string>> keys;
        auto value = Json::parse(text, [&keys](int depth, Json::parse_event_t event, Json& parsed) {
            if (depth > 64) throw std::invalid_argument("JSON depth exceeds 64");
            if (event == Json::parse_event_t::object_start) keys.emplace_back();
            if (event == Json::parse_event_t::key && !keys.back().insert(parsed.get<std::string>()).second) throw std::invalid_argument("Duplicate JSON key");
            if (event == Json::parse_event_t::object_end) keys.pop_back();
            return true;
        });
        auto r = validate_command_value(value, 1024 * 1024); if (!r) return std::unexpected(r.error());
        return value;
    } catch (const Json::exception& e) { return std::unexpected(Error{ErrorCode::invalid_argument, e.what()}); }
      catch (const std::invalid_argument& e) { return std::unexpected(Error{ErrorCode::invalid_argument, e.what()}); }
}
namespace schema {
Json object(Json properties, Json required) { return {{"type", "object"}, {"properties", std::move(properties)}, {"required", std::move(required)}, {"additionalProperties", false}}; }
Json array(Json items, std::size_t minimum, std::size_t maximum) { return {{"type", "array"}, {"items", std::move(items)}, {"minItems", minimum}, {"maxItems", maximum}}; }
Json string(std::size_t minimum, std::size_t maximum) { return {{"type", "string"}, {"minLength", minimum}, {"maxLength", maximum}}; }
Json integer() { return {{"type", "integer"}, {"x-dk-integer-token", true}}; }
Json number() { return {{"type", "number"}}; }
Json boolean() { return {{"type", "boolean"}}; }
Json nullable(Json value) { const auto type = value.at("type"); value["type"] = type.is_array() ? type : Json::array({type}); value["type"].push_back("null"); return value; }
Result<void> check(const Json& definition) { auto r = validate_command_value(definition, 1024 * 1024); if (!r) return r; return check_schema(definition, 0); }
Result<void> validate(const Json& definition, const Json& value) { auto r = check(definition); if (!r) return r; r = validate_command_value(value, 16 * 1024 * 1024); if (!r) return r; return validate_value(definition, value, "$" ); }
} // namespace schema
} // namespace dk
