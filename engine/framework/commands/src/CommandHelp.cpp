#include "CommandHelp.hpp"
#include <algorithm>

namespace dk::detail {
namespace {
Result<void> invalid(std::string message) {
    return std::unexpected(Error{ErrorCode::invalid_argument,"Invalid command help: " + std::move(message)});
}
bool text(const Json& value, std::size_t limit, bool multiline = false) {
    if (!value.is_string()) return false;
    const auto& s = value.get_ref<const std::string&>();
    if (s.empty() || s.size() > limit) return false;
    return std::ranges::none_of(s, [multiline](unsigned char c) {
        return c == 0x7f || (c < 0x20 && !(multiline && (c == '\n' || c == '\t')));
    });
}
const Json* schema_at(const Json& root, const std::string& path) {
    if (path.size() > 1024) return nullptr;
    try {
        Json::json_pointer pointer{path};
        std::vector<std::string> tokens;
        while (!pointer.empty()) { tokens.push_back(pointer.back()); pointer.pop_back(); }
        std::ranges::reverse(tokens);
        const auto* node = &root;
        for (std::size_t i = 0; i < tokens.size();) {
            if (tokens[i] == "properties" && i+1 < tokens.size() && node->contains("properties")) {
                const auto& properties = node->at("properties");
                if (!properties.contains(tokens[i+1])) return nullptr;
                node = &properties.at(tokens[i+1]); i += 2;
            } else if (tokens[i] == "items" && node->contains("items")) {
                node = &node->at("items"); ++i;
            } else return nullptr;
        }
        return node;
    } catch (const Json::exception&) { return nullptr; }
}
}
Result<void> validate_command_help(const Json& parameters, const Json& help,
    const std::set<std::string, std::less<>>& providers) {
    if (auto valid = validate_command_value(help,64*1024); !valid)
        return invalid(valid.error().message);
    if (!help.is_object() || help.size()!=2 || !help.contains("version") || !help.contains("fields") ||
        !help["version"].is_number_integer() || help["version"]!=1 || !help["fields"].is_object())
        return invalid("expected {version:1,fields:{schema_pointer:entry}}");
    for (const auto& [path, entry] : help["fields"].items()) {
        const auto* target = schema_at(parameters,path);
        if (!target) return invalid("reference is not a parameter schema node: " + path);
        if (!entry.is_object() || entry.empty()) return invalid("expected a nonempty field entry at " + path);
        for (const auto& [key, value] : entry.items()) {
            bool valid = false;
            if (key=="label") valid=text(value,256);
            else if (key=="description") valid=text(value,4096,true);
            else if (key=="unit") valid=text(value,64);
            else if (key=="default") valid=bool(schema::validate(*target,value));
            else if (key=="examples") {
                valid=value.is_array() && value.size()<=8;
                if (valid) for (const auto& sample : value) if (!schema::validate(*target,sample)) { valid=false; break; }
            } else if (key=="aliases") {
                valid=value.is_array() && value.size()<=16;
                std::set<std::string> seen;
                if (valid) for (const auto& alias : value) {
                    if (!text(alias,128) || !seen.insert(alias.get<std::string>()).second) { valid=false; break; }
                }
            } else if (key=="completion_provider") {
                valid=value.is_string() && providers.contains(value.get_ref<const std::string&>());
            }
            if (!valid) return invalid("invalid " + key + " at " + path);
        }
    }
    return {};
}
} // namespace dk::detail
