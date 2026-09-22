#pragma once

#include <dk/core/Result.hpp>

#include <fmt/format.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string_view>
#include <utility>

namespace dk {

enum class LogLevel : std::uint8_t {
    trace,
    debug,
    info,
    warning,
    error,
    critical,
    off,
};

struct LogConfig {
    LogLevel minimum_level = LogLevel::info;
    bool stderr_enabled = true;
    std::filesystem::path file_path{};
};

class Logger {
public:
    [[nodiscard]] static Result<std::unique_ptr<Logger>> create(const LogConfig& config = {});
    ~Logger() noexcept;

    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;
    Logger(Logger&&) = delete;
    Logger& operator=(Logger&&) = delete;

    [[nodiscard]] bool enabled(LogLevel level) const noexcept;
    [[nodiscard]] Result<void> write(
        LogLevel level, std::string_view module, std::string_view message);
    [[nodiscard]] Result<void> flush();

    template <typename... Args>
    [[nodiscard]] Result<void> log(LogLevel level, std::string_view module,
        fmt::format_string<Args...> format, Args&&... args)
    {
        auto valid = validate_entry(level, module);
        if (!valid) {
            return valid;
        }
        if (!enabled(level)) {
            return {};
        }
        return write(level, module, fmt::format(format, std::forward<Args>(args)...));
    }

private:
    struct Impl;
    explicit Logger(std::unique_ptr<Impl> impl) noexcept;
    [[nodiscard]] static Result<void> validate_entry(LogLevel level, std::string_view module);
    std::unique_ptr<Impl> impl_;
};

} // namespace dk
