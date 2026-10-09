#include <dk/scripting/Luau.hpp>
#include <dk/runtime/Runtime.hpp>
#include <Luau/Compiler.h>
#include <lua.h>
#include <lualib.h>
#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <exception>
#include <memory>

namespace dk {
namespace {
constexpr double max_exact_integer = 9007199254740991.0;
constexpr int integer_tag = 1;
constexpr const char* array_metatable = "dk.array";
char null_token;

enum class Stop { none, cancelled, timeout, interrupt_limit, command_limit, memory_limit };
struct Invocation {
    Runtime& runtime;
    LuauOptions options;
    std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
    std::exception_ptr fatal;
    Stop stopped = Stop::none;
    std::uint64_t interrupts = 0;
    std::uint64_t commands = 0;
    std::size_t vm_bytes = 0;
    bool native_oom = false;

    bool poll() {
        if (stopped == Stop::none) {
            if (options.stop.stop_requested()) stopped = Stop::cancelled;
            else if (std::chrono::steady_clock::now() - started >= options.limits.timeout) stopped = Stop::timeout;
        }
        return stopped != Stop::none || fatal || native_oom;
    }
};
Invocation& invocation(lua_State* state) {
    return *static_cast<Invocation*>(lua_callbacks(state)->userdata);
}
int suspend(lua_State* state) {
    if (lua_isyieldable(state)) return lua_break(state);
    luaL_error(state, "Luau execution stopped");
}
void interrupt(lua_State* state, int gc) {
    auto& context = invocation(state);
    if (!context.poll() && gc < 0 && ++context.interrupts > context.options.limits.max_interrupts)
        context.stopped = Stop::interrupt_limit;
    // GC cannot yield or raise an error; latch only until the next VM safepoint.
    if (gc < 0 && context.poll()) (void)suspend(state);
}
void* allocate(void* user, void* pointer, std::size_t old_size, std::size_t size) noexcept {
    auto& context = *static_cast<Invocation*>(user);
    if (!pointer) old_size = 0;
    if (size == 0) {
        std::free(pointer);
        context.vm_bytes -= old_size;
        return nullptr;
    }
    const auto remaining = context.vm_bytes - old_size;
    if (size > context.options.limits.max_vm_bytes - remaining) {
        if (context.stopped == Stop::none) context.stopped = Stop::memory_limit;
        return nullptr;
    }
    auto* result = std::realloc(pointer, size);
    if (!result) {
        context.native_oom = true;
        return nullptr;
    }
    context.vm_bytes = remaining + size;
    return result;
}
// Luau turns escaping std::exception into a catchable script error. Remember
// host OOM independently so pcall/coroutines cannot consume a fatal host fault.
template <lua_CFunction Function> int host_boundary(lua_State* state) {
    auto& context = invocation(state);
    if (context.poll()) return suspend(state);
    try {
        const auto results = Function(state);
        return context.poll() ? suspend(state) : results;
    }
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
    std::size_t bytes = 0;
    std::vector<const void*> ancestors;

    void consume(std::size_t size) {
        if (size > 1024 * 1024 - bytes) reject("Command input exceeds logical byte limit");
        bytes += size;
    }
    std::string text(lua_State* state, int index) {
        std::size_t size = 0;
        const auto* data = lua_tolstring(state, index, &size);
        consume(size);
        return std::string(data, size);
    }

    Json read(lua_State* state, int index, unsigned depth = 0) {
        if (depth > 64 || ++nodes > 200000) reject("Command value exceeds depth/node limit");
        consume(1);
        stack_space(state);
        index = lua_absindex(state, index);
        switch (lua_type(state, index)) {
        case LUA_TBOOLEAN: return lua_toboolean(state, index) != 0;
        case LUA_TSTRING: return text(state, index);
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
                const auto key = text(state, -2);
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
bool allowed(std::string_view name, LuauAccess access) {
    constexpr std::array queries = {"commands.list", "commands.describe", "runtime.capabilities", "tasks.list",
        "tasks.get", "scene.query", "entity.get", "history.status", "simulation.query", "simulation.particles"};
    if (std::find(queries.begin(), queries.end(), name) != queries.end()) return true;
    if (access == LuauAccess::query) return false;
    if (access == LuauAccess::edit && (name == "scene.load" || name == "scene.save" || name == "project.save"))
        return false;
    constexpr std::array names = {
        "commands.list", "commands.describe", "runtime.capabilities", "tasks.list", "tasks.get",
        "scene.new", "scene.load", "scene.query", "scene.save", "scene.transaction", "project.save",
        "entity.create", "entity.get", "entity.delete", "entity.set_name", "entity.set_transform",
        "entity.set_parent", "entity.set_assets", "history.status", "history.undo", "history.redo",
        "simulation.start", "simulation.pause", "simulation.resume", "simulation.step", "simulation.stop"};
    return std::find(names.begin(), names.end(), name) != names.end();
}
int command(lua_State* state) {
    auto& context = invocation(state);
    if (context.commands >= context.options.limits.max_commands) {
        context.stopped = Stop::command_limit;
        return suspend(state);
    }
    ++context.commands;
    const int arguments = lua_gettop(state);
    Json task = nullptr;
    Json response;
    try {
        if (arguments < 1 || arguments > 2 || lua_type(state, 1) != LUA_TSTRING)
            reject("dk.command expects a method string and optional parameter table");
        if (lua_objlen(state, 1) > 96) reject("Command method exceeds 96 bytes");
        const auto method = string_value(state, 1);
        if (!allowed(method, context.options.access)) {
            response = failure({ErrorCode::not_supported, "Command is not available to scene scripts", {method}}, task);
        } else {
            auto parameters = arguments < 2 || lua_isnil(state, 2) ? Json::object() : ValueReader{}.read(state, 2);
            if (context.poll()) return suspend(state);
            auto execution = [&]() -> Result<CommandExecution> {
                try { return context.runtime.dispatch(method, parameters); }
                catch (...) { context.fatal = std::current_exception(); }
                luaL_error(state, "Script host command failed");
            }();
            if (context.poll()) return suspend(state);
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
const char* stop_name(Stop stop) {
    switch (stop) {
    case Stop::cancelled: return "luau.cancelled";
    case Stop::timeout: return "luau.timeout";
    case Stop::interrupt_limit: return "luau.interrupt_limit";
    case Stop::command_limit: return "luau.command_limit";
    case Stop::memory_limit: return "luau.memory_limit";
    default: return "luau.stopped";
    }
}
Result<void> stopped_result(const Invocation& context, std::string_view chunk_name) {
    if (context.fatal) std::rethrow_exception(context.fatal);
    if (context.native_oom) throw std::bad_alloc{};
    const auto* reason = stop_name(context.stopped);
    return std::unexpected(Error{ErrorCode::invalid_state, std::string(reason) + ": " + std::string(chunk_name),
                                 {reason, std::string(chunk_name)}});
}
bool valid_options(const LuauOptions& options) {
    const auto& limits = options.limits;
    return limits.timeout.count() >= 1 && limits.timeout.count() <= 600000 &&
        limits.max_interrupts >= 1 && limits.max_interrupts <= 1000000000 &&
        limits.max_commands >= 1 && limits.max_commands <= 100000 &&
        limits.max_vm_bytes >= 256 * 1024 && limits.max_vm_bytes <= 256 * 1024 * 1024 &&
        limits.max_source_bytes >= 1 && limits.max_source_bytes <= 1024 * 1024 &&
        (options.access == LuauAccess::query || options.access == LuauAccess::edit || options.access == LuauAccess::project);
}
} // namespace

Result<void> run_luau(Runtime& runtime, std::string_view source, std::string_view chunk_name, const LuauOptions& options) {
    if (!valid_options(options))
        return std::unexpected(Error{ErrorCode::invalid_argument, "Invalid Luau execution options"});
    Invocation context{runtime, options};
    if (context.poll()) return stopped_result(context, chunk_name);
    if (chunk_name.empty() || chunk_name.find('\0') != std::string_view::npos)
        return std::unexpected(Error{ErrorCode::invalid_argument, "Script chunk name must be nonempty without NUL"});
    if (source.size() > options.limits.max_source_bytes)
        return std::unexpected(Error{ErrorCode::invalid_argument, "Luau source exceeds byte limit"});
    // There is deliberately no bytecode overload. Even binary input is compiled as source.
    const auto bytecode = Luau::compile(std::string(source));
    if (context.poll()) return stopped_result(context, chunk_name);
    if (bytecode.size() > 16 * 1024 * 1024)
        return std::unexpected(Error{ErrorCode::invalid_argument, "Luau bytecode exceeds byte limit"});
    const std::string name = "@" + std::string(chunk_name);
    std::unique_ptr<lua_State, decltype(&lua_close)> state(lua_newstate(allocate, &context), lua_close);
    if (!state) return stopped_result(context, chunk_name);
    lua_callbacks(state.get())->userdata = &context;
    const auto setup = lua_cpcall(state.get(), [](lua_State* vm) {
        initialize(vm, invocation(vm));
        return 0;
    }, nullptr);
    if (context.poll()) return stopped_result(context, chunk_name);
    if (setup != 0) {
        if (setup == LUA_ERRMEM) throw std::bad_alloc{};
        return std::unexpected(Error{ErrorCode::internal_error, diagnostic(state.get()), {std::string(chunk_name), "Luau setup"}});
    }
    const auto loaded = luau_load(state.get(), name.c_str(), bytecode.data(), bytecode.size(), 0);
    if (context.poll()) return stopped_result(context, chunk_name);
    if (loaded != 0)
        return std::unexpected(Error{ErrorCode::invalid_argument, diagnostic(state.get()), {std::string(chunk_name), "Luau compile/load"}});
    lua_callbacks(state.get())->interrupt = interrupt;
    const auto status = lua_resume(state.get(), nullptr, 0);
    if (context.poll()) return stopped_result(context, chunk_name);
    if (status == LUA_ERRMEM) throw std::bad_alloc{};
    if (status == LUA_YIELD || status == LUA_BREAK)
        return std::unexpected(Error{ErrorCode::invalid_state, "Top-level Luau yield has no scheduler", {std::string(chunk_name)}});
    if (status != 0)
        return std::unexpected(Error{ErrorCode::invalid_state, diagnostic(state.get()), {std::string(chunk_name), "Luau execution"}});
    return {};
}
} // namespace dk
