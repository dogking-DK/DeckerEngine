#include <dk/editor/Console.hpp>
#include <algorithm>
#include <charconv>
#include <cstdint>
#include <set>
#include <vector>

namespace dk::editor {
namespace {
bool space(char c) { return c == ' ' || c == '\t'; }
bool letter(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
bool digit(char c) { return c >= '0' && c <= '9'; }
bool name_char(char c) { return letter(c) || digit(c) || c == '_'; }
enum class Number { none, integer, floating };
Number number_kind(std::string_view s) {
    std::size_t p = 0;
    if (p < s.size() && s[p] == '-') ++p;
    if (p == s.size() || !digit(s[p])) return Number::none;
    if (s[p] == '0') ++p;
    else while (p < s.size() && digit(s[p])) ++p;
    auto kind = Number::integer;
    if (p < s.size() && s[p] == '.') {
        kind = Number::floating;
        const auto start = ++p;
        while (p < s.size() && digit(s[p])) ++p;
        if (p == start) return Number::none;
    }
    if (p < s.size() && (s[p] == 'e' || s[p] == 'E')) {
        kind = Number::floating; ++p;
        if (p < s.size() && (s[p] == '+' || s[p] == '-')) ++p;
        const auto start = p;
        while (p < s.size() && digit(s[p])) ++p;
        if (p == start) return Number::none;
    }
    return p == s.size() ? kind : Number::none;
}
struct ParseFailure { Error error; };
class Parser {
public:
    explicit Parser(std::string_view text) : text_(text) {}
    ConsoleCommand parse() {
        input(); skip();
        if (at_ == text_.size()) fail("Expected a command", at_);
        ConsoleCommand command;
        const auto start = at_;
        if (text_[at_] == '{') {
            auto envelope = json_value(0);
            skip();
            if (at_ != text_.size()) fail("Unexpected text after JSON command", at_);
            if (!envelope.is_object() || envelope.size() != 2 || !envelope.contains("method") ||
                !envelope["method"].is_string() || !envelope.contains("params") || !envelope["params"].is_object())
                fail("Expected JSON {method,params}", start);
            command = {envelope["method"].get<std::string>(), std::move(envelope["params"])};
        } else {
            while (at_ < text_.size() && !space(text_[at_])) ++at_;
            command.method = text_.substr(start, at_ - start);
            method(command.method, start);
            command.params = Json::object();
            skip();
            while (at_ < text_.size()) {
                const auto key_start = at_;
                if (!letter(text_[at_]) && text_[at_] != '_')
                    fail("Expected a named parameter, for example limit=20", at_);
                while (at_ < text_.size() && name_char(text_[at_])) ++at_;
                const std::string key{text_.substr(key_start, at_ - key_start)};
                skip();
                if (at_ == text_.size() || text_[at_] != '=')
                    fail("Expected key=value; positional and dotted parameters are not supported", at_);
                if (command.params.contains(key)) fail("Duplicate parameter", key_start);
                ++at_; skip();
                if (at_ == text_.size()) fail("Missing parameter value", at_);
                command.params[key] = value();
                if (at_ < text_.size() && !space(text_[at_])) fail("Expected whitespace after value", at_);
                skip();
            }
        }
        method(command.method, start);
        // Validate the full envelope equally for both syntaxes, not just each value.
        auto valid = validate_command_value(Json{{"method",command.method},{"params",command.params}}, 1024 * 1024);
        if (!valid) fail("Command value exceeds structural or encoding limits", start, true);
        return command;
    }
private:
    std::string_view text_;
    std::size_t at_ = 0;
    [[noreturn]] void fail(std::string message, std::size_t offset, bool limit = false, bool byte_only = false) const {
        offset = std::min(offset, text_.size());
        if (byte_only) message += " at byte " + std::to_string(offset);
        else {
            std::size_t column = 1;
            for (std::size_t i = 0; i < offset; ++i)
                if ((static_cast<unsigned char>(text_[i]) & 0xc0) != 0x80) ++column;
            message += " at column " + std::to_string(column);
        }
        throw ParseFailure{{ErrorCode::invalid_argument, std::move(message),
            {limit ? "console.limit" : "console.syntax", "byte=" + std::to_string(offset)}}};
    }
    void input() const {
        if (text_.size() > console_input_limit) fail("Console input exceeds 8192 bytes", console_input_limit, true, true);
        // Validate before computing Unicode columns or passing raw strings to Json.
        for (std::size_t i = 0; i < text_.size();) {
            const auto c = static_cast<unsigned char>(text_[i]);
            if (c < 0x80) {
                if (c < 0x20 && c != '\t') fail("Console input must be a single line without control bytes", i);
                ++i; continue;
            }
            const auto start = i;
            const unsigned count = c >= 0xc2 && c <= 0xdf ? 2 : c >= 0xe0 && c <= 0xef ? 3 : c >= 0xf0 && c <= 0xf4 ? 4 : 0;
            if (!count || i + count > text_.size()) fail("Invalid UTF-8", start, false, true);
            std::uint32_t code = c & (0x7f >> count);
            for (unsigned j = 1; j < count; ++j) {
                const auto next = static_cast<unsigned char>(text_[i+j]);
                if ((next & 0xc0) != 0x80) fail("Invalid UTF-8", i+j, false, true);
                code = (code << 6) | (next & 0x3f);
            }
            if ((count == 3 && code < 0x800) || (count == 4 && code < 0x10000) ||
                (code >= 0xd800 && code <= 0xdfff) || code > 0x10ffff) fail("Invalid UTF-8", start, false, true);
            i += count;
        }
    }
    void skip() { while (at_ < text_.size() && space(text_[at_])) ++at_; }
    void method(std::string_view name, std::size_t start) const {
        if (name.empty() || name.size() > 96 || !letter(name.front())) fail("Invalid command name (1-96 ASCII characters)", start);
        for (std::size_t i = 1; i < name.size(); ++i)
            if (!name_char(name[i]) && name[i] != '.') fail("Invalid command name", start);
    }
    void integer(std::string_view token, std::size_t start) const {
        if (number_kind(token) != Number::integer) return;
        std::from_chars_result result;
        if (token.front() == '-') {
            std::int64_t value;
            result = std::from_chars(token.data(), token.data()+token.size(), value);
        } else {
            std::uint64_t value;
            result = std::from_chars(token.data(), token.data()+token.size(), value);
        }
        if (result.ec != std::errc{}) fail("Integer exceeds int64/uint64 range", start, true);
    }
    Json decode(std::size_t start, std::size_t end) const {
        try { return Json::parse(text_.substr(start, end-start)); }
        catch (const Json::parse_error& e) {
            fail("Invalid JSON value", start + std::min(e.byte ? e.byte-1 : 0, end-start));
        } catch (const Json::out_of_range&) { fail("JSON number is out of range", start, true); }
    }
    void quoted() {
        const auto start = at_++;
        while (at_ < text_.size()) {
            if (text_[at_] == '"') { ++at_; return; }
            if (text_[at_] == '\\') {
                ++at_;
                if (at_ == text_.size()) break;
            }
            ++at_;
        }
        fail("Unclosed double quote", start);
    }
    Json json_value(std::size_t base_depth) {
        const auto start = at_;
        if (text_[at_] == '"') { quoted(); return decode(start, at_); }
        struct Frame { char opening; std::size_t start; std::set<std::string> keys; };
        std::vector<Frame> stack;
        // Preflight before building a DOM: nesting, duplicate decoded keys and
        // integer tokens (nlohmann otherwise converts overflowing integers to double).
        do {
            const char c = text_[at_];
            if (space(c) || c == ',' || c == ':') { ++at_; continue; }
            if (c == '}' || c == ']') {
                if (stack.empty() || stack.back().opening != (c == '}' ? '{' : '['))
                    fail("Mismatched JSON bracket", at_);
                stack.pop_back(); ++at_;
                continue;
            }
            if (base_depth + stack.size() > 64) fail("JSON depth exceeds 64", at_, true);
            if (c == '{' || c == '[') { stack.push_back({c,at_,{}}); ++at_; }
            else if (c == '"') {
                const auto quote = at_;
                quoted();
                const auto decoded = decode(quote, at_);
                auto next = at_;
                while (next < text_.size() && space(text_[next])) ++next;
                if (!stack.empty() && stack.back().opening == '{' && next < text_.size() && text_[next] == ':')
                    if (!stack.back().keys.insert(decoded.get<std::string>()).second) fail("Duplicate JSON key", quote);
            } else {
                const auto token = at_;
                while (at_ < text_.size() && !space(text_[at_]) &&
                    std::string_view{",:{}[]\""}.find(text_[at_]) == std::string_view::npos) ++at_;
                integer(text_.substr(token, at_-token), token);
            }
        } while (!stack.empty() && at_ < text_.size());
        if (!stack.empty()) fail("Unclosed JSON bracket", stack.back().start);
        return decode(start, at_);
    }
    Json value() {
        const auto start = at_;
        const char c = text_[at_];
        if (c == '"' || c == '[' || c == '{') return json_value(2);
        if (c == '\'') {
            ++at_;
            while (at_ < text_.size() && text_[at_] != '\'') ++at_;
            if (at_ == text_.size()) fail("Unclosed single quote", start);
            const std::string result{text_.substr(start+1, at_-start-1)};
            ++at_; return result;
        }
        while (at_ < text_.size() && !space(text_[at_])) {
            if (std::string_view{"\"'[]{}=;&|"}.find(text_[at_]) != std::string_view::npos)
                fail("Quote this value; shell operators and command chaining are not supported", at_);
            ++at_;
        }
        const auto token = text_.substr(start, at_-start);
        if (token == "true" || token == "false" || token == "null" || number_kind(token) != Number::none) {
            integer(token, start);
            return decode(start, at_);
        }
        return std::string{token};
    }
};
} // namespace
Result<ConsoleCommand> parse_console_command(std::string_view text) {
    try { return Parser{text}.parse(); }
    catch (const ParseFailure& error) { return std::unexpected(error.error); }
}
} // namespace dk::editor
