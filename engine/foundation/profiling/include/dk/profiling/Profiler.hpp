#pragma once

#ifndef DK_ENABLE_PROFILING
#define DK_ENABLE_PROFILING 0
#endif

#if DK_ENABLE_PROFILING
#include <tracy/Tracy.hpp>
#include <cstdint>
#include <string_view>

#ifndef TRACY_ENABLE
#error "dk::profiling requires an enabled Tracy client target"
#endif
#ifndef DK_PROFILE_CALLSTACK_DEPTH
#define DK_PROFILE_CALLSTACK_DEPTH 0
#endif

// Source locations and the RAII zone must be created at the call site.
// Use one zone per lexical scope; names must have static lifetime.
#define DK_PROFILE_ZONE(name) \
    SuppressVarShadowWarning(ZoneScopedNS((name), DK_PROFILE_CALLSTACK_DEPTH))
#define DK_PROFILE_ZONE_VALUE(value) ZoneValue(static_cast<std::uint64_t>(value))
// Extend a temporary string's lifetime through Tracy's copy. Empty text is
// ignored; truncate to Tracy's < 65535-byte limit (byte, not Unicode boundary).
#define DK_PROFILE_ZONE_TEXT(text) do { \
    const auto& dk_profile_text_storage = (text); \
    const auto dk_profile_text_view = std::string_view{dk_profile_text_storage}.substr(0, 65534); \
    if (!dk_profile_text_view.empty()) { \
        ZoneText(dk_profile_text_view.data(), dk_profile_text_view.size()); \
    } \
} while (false)
#define DK_PROFILE_FRAME(name) FrameMarkNamed(name)
#define DK_PROFILE_THREAD_NAME(name) ::dk::profiling::set_thread_name(name)
#else
// Deliberately do not mention the arguments: disabled markup has no side effects.
#define DK_PROFILE_ZONE(name) ((void)0)
#define DK_PROFILE_ZONE_VALUE(value) ((void)0)
#define DK_PROFILE_ZONE_TEXT(text) ((void)0)
#define DK_PROFILE_FRAME(name) ((void)0)
#define DK_PROFILE_THREAD_NAME(name) ((void)0)
#endif

namespace dk::profiling {

[[nodiscard]] inline constexpr bool enabled() noexcept { return DK_ENABLE_PROFILING != 0; }

#if DK_ENABLE_PROFILING
// name is a non-null, null-terminated thread name; Tracy copies it.
void set_thread_name(const char* name);
[[nodiscard]] bool is_connected() noexcept;
#else
inline void set_thread_name(const char*) noexcept {}
[[nodiscard]] inline constexpr bool is_connected() noexcept { return false; }
#endif

} // namespace dk::profiling
