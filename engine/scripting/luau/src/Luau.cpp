#include <dk/scripting/Luau.hpp>
#include <dk/runtime/Runtime.hpp>
#include <Luau/Compiler.h>
#include <lua.h>
#include <lualib.h>
#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <exception>
#include <memory>

namespace dk {
namespace {
constexpr double max_exact_integer = 9007199254740991.0;
constexpr int integer_tag = 1;
constexpr const char* array_metatable = "dk.array";
char null_token;

struct Invocation {
    Runtime& runtime;
    std::exception_ptr fatal;
};
Invocation& invocation(lua_State* state) {
    return *static_cast<Invocation*>(lua_callbacks(state)->userdata);
}
// Luau turns escaping std::exception into a catchable script error. Remember
// host OOM independently so pcall/coroutines cannot consume a fatal host fault.
template <lua_CFunction Function> int host_boundary(lua_State* state) {
    auto& context = invocation(state);
    if (context.fatal) luaL_error(state, "Script host has failed");
    try { return Function(state); }
    catch (const std::bad_alloc&) { context.fatal = std::current_exception(); }
    luaL_error(state, "Script host allocation failed");
}

struct ExactInteger {
    std::uint64_t magnitude;
    bool negative;
};
struct BridgeError { Error error; };
[[noreturn]] void reject(std::string message) {
    throw BridgeError{{ErrorCode::invalid_argument, std::move(message)}};
}
void stack_space(lua_State* state) {
    if (!lua_checkstack(state, 8)) reject("Luau command value exceeds VM stack capacity");
}
std::string string_value(lua_State* state, int index) {
    std::size_t size = 0;
    const auto* data = lua_tolstring(state, index, &size);
    return std::string(data, size);
}
Json integer_json(const ExactInteger& value) {
    if (value.negative)
        return -static_cast<std::int64_t>(value.magnitude - 1) - 1;
    return value.magnitude;
}
void push_integer(lua_State* state, ExactInteger value) {
    auto* stored = static_cast<ExactInteger*>(lua_newuserdatataggedwithmetatable(state, sizeof(ExactInteger), integer_tag));
    std::construct_at(stored, value);
}
int integer_text(lua_State* state) {
    auto* value = static_cast<ExactInteger*>(lua_touserdatatagged(state, 1, integer_tag));
    if (!value) luaL_error(state, "Expected dk.integer");
    const auto text = integer_json(*value).dump();
    lua_pushlstring(state, text.data(), text.size());
    return 1;
}
int make_integer(lua_State* state) {
    if (lua_gettop(state) != 1 || lua_type(state, 1) != LUA_TSTRING)
        luaL_error(state, "dk.integer expects one decimal string");
    const auto text = string_value(state, 1);
    ExactInteger value{};
    bool valid = false;
    if (!text.empty() && text[0] == '-') {
        std::int64_t number{};
        const auto parsed = std::from_chars(text.data(), text.data() + text.size(), number);
        valid = parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size();
        value.negative = number < 0;
        value.magnitude = number < 0 ? static_cast<std::uint64_t>(-(number + 1)) + 1 : 0;
    } else {
        const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value.magnitude);
        valid = parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size();
    }
    if (!valid) luaL_error(state, "dk.integer expects an int64/uint64 decimal string");
    push_integer(state, value);
    return 1;
}
int make_array(lua_State* state) {
    if (lua_gettop(state) != 0) luaL_error(state, "dk.array expects no arguments");
    lua_newtable(state);
    luaL_getmetatable(state, array_metatable);
    lua_setmetatable(state, -2);
    return 1;
}

struct ValueReader {
    std::size_t nodes = 0;
    std::vector<const void*> ancestors;

    Json read(lua_State* state, int index, unsigned depth = 0) {
        if (depth > 64 || ++nodes > 200000) reject("Command value exceeds depth/node limit");
        stack_space(state);
        index = lua_absindex(state, index);
        switch (lua_type(state, index)) {
        case LUA_TBOOLEAN: return lua_toboolean(state, index) != 0;
        case LUA_TSTRING: return string_value(state, index);
        case LUA_TNUMBER: {
            const auto number = lua_tonumber(state, index);
            if (!std::isfinite(number)) reject("Command numbers must be finite");
            if (std::trunc(number) == number) {
                if (std::abs(number) > max_exact_integer)
                    reject("Integer number exceeds exact range; use dk.integer(decimal_string)");
                return static_cast<std::int64_t>(number);
            }
            return number;
        }
        case LUA_TLIGHTUSERDATA:
            if (lua_tolightuserdata(state, index) == &null_token) return nullptr;
            break;
        case LUA_TUSERDATA:
            if (auto* value = static_cast<ExactInteger*>(lua_touserdatatagged(state, index, integer_tag)))
                return integer_json(*value);
            break;
        case LUA_TTABLE: return table(state, index, depth);
        default: break;
        }
        reject("Unsupported Luau command value type");
    }

    Json table(lua_State* state, int index, unsigned depth) {
        bool marked_array = false;
        if (lua_getmetatable(state, index)) {
            luaL_getmetatable(state, array_metatable);
            marked_array = lua_rawequal(state, -1, -2) != 0;
            lua_pop(state, 2);
            if (!marked_array) reject("Command tables must not have a metatable");
        }
        const auto* identity = lua_topointer(state, index);
        if (std::find(ancestors.begin(), ancestors.end(), identity) != ancestors.end())
            reject("Cyclic command table");
        ancestors.push_back(identity);
        bool numeric = false, named = false;
        std::size_t count = 0, max_index = 0;
        lua_pushnil(state);
        while (lua_next(state, index)) {
            if (++count > 200000) reject("Command table exceeds node limit");
            if (lua_type(state, -2) == LUA_TSTRING) named = true;
            else if (lua_type(state, -2) == LUA_TNUMBER) {
                const auto key = lua_tonumber(state, -2);
                if (!std::isfinite(key) || key < 1 || key > 200000 || std::trunc(key) != key)
                    reject("Array keys must be consecutive positive integers");
                numeric = true;
                max_index = std::max(max_index, static_cast<std::size_t>(key));
            } else reject("Command table keys must be strings or array indices");
            lua_pop(state, 1);
        }
        if ((named && (numeric || marked_array)) || (numeric && max_index != count))
            reject("Mixed or sparse command table");
        Json result = numeric || marked_array ? Json::array() : Json::object();
        if (result.is_array()) {
            for (std::size_t i = 1; i <= count; ++i) {
                lua_rawgeti(state, index, static_cast<int>(i));
                result.push_back(read(state, -1, depth + 1));
                lua_pop(state, 1);
            }
        } else {
            lua_pushnil(state);
            while (lua_next(state, index)) {
                const auto key = string_value(state, -2);
                result[key] = read(state, -1, depth + 1);
                lua_pop(state, 1);
            }
        }
        ancestors.pop_back();
        return result;
    }
};

void push_json(lua_State* state, const Json& value) {
    stack_space(state);
    if (value.is_null()) lua_pushlightuserdata(state, &null_token);
    else if (value.is_boolean()) lua_pushboolean(state, value.get<bool>());
    else if (value.is_string()) {
        const auto& text = value.get_ref<const std::string&>();
        lua_pushlstring(state, text.data(), text.size());
    } else if (value.is_number_unsigned()) {
        const auto number = value.get<std::uint64_t>();
        if (number > static_cast<std::uint64_t>(max_exact_integer)) push_integer(state, {number, false});
        else lua_pushnumber(state, static_cast<double>(number));
    } else if (value.is_number_integer()) {
        const auto number = value.get<std::int64_t>();
        if (number < -static_cast<std::int64_t>(max_exact_integer))
            push_integer(state, {static_cast<std::uint64_t>(-(number + 1)) + 1, true});
        else if (number > static_cast<std::int64_t>(max_exact_integer))
            push_integer(state, {static_cast<std::uint64_t>(number), false});
        else lua_pushnumber(state, static_cast<double>(number));
    } else if (value.is_number()) lua_pushnumber(state, value.get<double>());
    else if (value.is_array()) {
        lua_createtable(state, static_cast<int>(value.size()), 0);
        luaL_getmetatable(state, array_metatable);
        lua_setmetatable(state, -2);
        int index = 1;
        for (const auto& item : value) {
            push_json(state, item);
            lua_rawseti(state, -2, index++);
        }
    } else {
        lua_createtable(state, 0, static_cast<int>(value.size()));
        for (const auto& [key, item] : value.items()) {
            lua_pushlstring(state, key.data(), key.size());
            push_json(state, item);
            lua_rawset(state, -3);
        }
    }
}
Json failure(const Error& error, const Json& task) {
    return {{"ok", false}, {"task_id", task},
            {"error", {{"code", static_cast<unsigned>(error.code)}, {"name", error_code_name(error.code)},
                       {"message", error.message}, {"context", error.context}}}};
}
bool allowed(std::string_view name) {
    constexpr std::array names = {
        "commands.list", "commands.describe", "runtime.capabilities", "tasks.list", "tasks.get",
        "scene.new", "scene.load", "scene.query", "scene.save", "scene.transaction", "project.save",
        "entity.create", "entity.get", "entity.delete", "entity.set_name", "entity.set_transform",
        "entity.set_parent", "entity.set_assets", "history.status", "history.undo", "history.redo"};
    return std::find(names.begin(), names.end(), name) != names.end();
}
int command(lua_State* state) {
    const int arguments = lua_gettop(state);
    Json task = nullptr;
    Json response;
    try {
        if (arguments < 1 || arguments > 2 || lua_type(state, 1) != LUA_TSTRING)
            reject("dk.command expects a method string and optional parameter table");
        const auto method = string_value(state, 1);
        if (!allowed(method)) {
            response = failure({ErrorCode::not_supported, "Command is not available to scene scripts", {method}}, task);
        } else {
            auto parameters = arguments < 2 || lua_isnil(state, 2) ? Json::object() : ValueReader{}.read(state, 2);
            auto& context = invocation(state);
            auto execution = [&]() -> Result<CommandExecution> {
                try { return context.runtime.dispatch(method, parameters); }
                catch (...) { context.fatal = std::current_exception(); }
                luaL_error(state, "Script host command failed");
            }();
            if (!execution) response = failure(execution.error(), task);
            else {
                task = execution->task_id.to_string();
                response = execution->result ? Json{{"ok", true}, {"task_id", task}, {"value", std::move(*execution->result)}}
                                             : failure(execution->result.error(), task);
            }
        }
        push_json(state, response);
    } catch (const BridgeError& error) {
        lua_settop(state, arguments);
        push_json(state, failure(error.error, task));
    }
    return 1;
}
void initialize(lua_State* state, Invocation& context) {
    lua_callbacks(state)->userdata = &context;
    luaL_openlibs(state);
    for (const auto* name : {"print", "require", "loadstring"}) {
        lua_pushnil(state);
        lua_setglobal(state, name);
    }
    luaL_newmetatable(state, array_metatable);
    lua_pushstring(state, "dk.array");
    lua_setfield(state, -2, "__metatable");
    lua_setreadonly(state, -1, true);
    lua_pop(state, 1);
    lua_newtable(state);
    lua_pushcfunction(state, host_boundary<integer_text>, "dk.integer.__tostring");
    lua_setfield(state, -2, "__tostring");
    lua_pushstring(state, "dk.integer");
    lua_setfield(state, -2, "__metatable");
    lua_setreadonly(state, -1, true);
    lua_setuserdatametatable(state, integer_tag);
    lua_newtable(state);
    lua_pushcfunction(state, host_boundary<command>, "dk.command");
    lua_setfield(state, -2, "command");
    lua_pushcfunction(state, host_boundary<make_array>, "dk.array");
    lua_setfield(state, -2, "array");
    lua_pushcfunction(state, host_boundary<make_integer>, "dk.integer");
    lua_setfield(state, -2, "integer");
    lua_pushlightuserdata(state, &null_token);
    lua_setfield(state, -2, "null");
    lua_setreadonly(state, -1, true);
    lua_setglobal(state, "dk");
    luaL_sandbox(state);
    luaL_sandboxthread(state);
}
std::string diagnostic(lua_State* state) {
    return lua_type(state, -1) == LUA_TSTRING ? string_value(state, -1) : "Luau raised a non-string error";
}
} // namespace

Result<void> run_luau(Runtime& runtime, std::string_view source, std::string_view chunk_name) {
    if (chunk_name.empty() || chunk_name.find('\0') != std::string_view::npos)
        return std::unexpected(Error{ErrorCode::invalid_argument, "Script chunk name must be nonempty without NUL"});
    // There is deliberately no bytecode overload. Even binary input is compiled as source.
    const auto bytecode = Luau::compile(std::string(source));
    const std::string name = "@" + std::string(chunk_name);
    Invocation context{runtime, {}};
    std::unique_ptr<lua_State, decltype(&lua_close)> state(luaL_newstate(), lua_close);
    if (!state) throw std::bad_alloc{};
    initialize(state.get(), context);
    if (luau_load(state.get(), name.c_str(), bytecode.data(), bytecode.size(), 0) != 0)
        return std::unexpected(Error{ErrorCode::invalid_argument, diagnostic(state.get()), {std::string(chunk_name), "Luau compile/load"}});
    const auto status = lua_pcall(state.get(), 0, 0, 0);
    if (context.fatal) std::rethrow_exception(context.fatal);
    if (status == LUA_ERRMEM) throw std::bad_alloc{};
    if (status != 0)
        return std::unexpected(Error{ErrorCode::invalid_state, diagnostic(state.get()), {std::string(chunk_name), "Luau execution"}});
    return {};
}
} // namespace dk
