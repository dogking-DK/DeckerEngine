#include <dk/core/Log.hpp>

#include <spdlog/logger.h>
#include <spdlog/sinks/ostream_sink.h>
#include <spdlog/sinks/stdout_sinks.h>

#include <cstdio>
#include <fstream>
#include <vector>

namespace dk {
namespace {

spdlog::level::level_enum native_level(LogLevel level) noexcept
{
    switch (level) {
    case LogLevel::trace: return spdlog::level::trace;
    case LogLevel::debug: return spdlog::level::debug;
    case LogLevel::info: return spdlog::level::info;
    case LogLevel::warning: return spdlog::level::warn;
    case LogLevel::error: return spdlog::level::err;
    case LogLevel::critical: return spdlog::level::critical;
    case LogLevel::off: return spdlog::level::off;
    }
    return spdlog::level::off;
}

} // namespace

struct Logger::Impl {
    // The backend's ostream sink must be destroyed before the stream.
    std::ofstream file;
    std::unique_ptr<spdlog::logger> backend;
    LogLevel minimum_level = LogLevel::info;
};

Logger::Logger(std::unique_ptr<Impl> impl) noexcept : impl_{std::move(impl)} {}

Result<std::unique_ptr<Logger>> Logger::create(const LogConfig& config)
{
    if (config.minimum_level > LogLevel::off) {
        return std::unexpected(Error{ErrorCode::invalid_argument, "Invalid log threshold"});
    }
    if (!config.stderr_enabled && config.file_path.empty()) {
        return std::unexpected(Error{ErrorCode::invalid_argument,
            "Logger requires stderr or a file output"});
    }

    try {
        auto impl = std::make_unique<Impl>();
        impl->minimum_level = config.minimum_level;
        std::vector<spdlog::sink_ptr> sinks;
        if (config.stderr_enabled) {
            sinks.push_back(std::make_shared<spdlog::sinks::stderr_sink_mt>());
        }
        if (!config.file_path.empty()) {
            impl->file.exceptions(std::ios::failbit | std::ios::badbit);
            impl->file.open(config.file_path, std::ios::out | std::ios::app | std::ios::binary);
            sinks.push_back(std::make_shared<spdlog::sinks::ostream_sink_mt>(impl->file));
        }

        impl->backend = std::make_unique<spdlog::logger>("dk", sinks.begin(), sinks.end());
        impl->backend->set_level(native_level(config.minimum_level));
        impl->backend->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] %v");
        impl->backend->set_error_handler([](const std::string& message) {
            // spdlog normally reports sink errors without notifying the caller.
            throw spdlog::spdlog_ex(message);
        });
        return std::unique_ptr<Logger>{new Logger{std::move(impl)}};
    } catch (const std::ios_base::failure& error) {
        return std::unexpected(Error{ErrorCode::io_error,
            "Failed to open log file", {error.what()}});
    } catch (const spdlog::spdlog_ex& error) {
        return std::unexpected(Error{ErrorCode::io_error,
            "Failed to initialize logger", {error.what()}});
    }
}

Logger::~Logger() noexcept
{
    try {
        impl_->backend->flush();
    } catch (...) {
        std::fputs("DeckerEngine: failed to flush logger during destruction.\n", stderr);
    }
}

bool Logger::enabled(LogLevel level) const noexcept
{
    return level < LogLevel::off && level >= impl_->minimum_level;
}

Result<void> Logger::validate_entry(LogLevel level, std::string_view module)
{
    if (level >= LogLevel::off) {
        return std::unexpected(Error{ErrorCode::invalid_argument, "Invalid log message level"});
    }
    if (module.empty() || module.find_first_of("\r\n") != std::string_view::npos
        || module.find('\0') != std::string_view::npos) {
        return std::unexpected(Error{ErrorCode::invalid_argument, "Invalid log module name"});
    }
    return {};
}

Result<void> Logger::write(LogLevel level, std::string_view module, std::string_view message)
{
    auto valid = validate_entry(level, module);
    if (!valid) {
        return valid;
    }
    if (!enabled(level)) {
        return {};
    }
    try {
        impl_->backend->log(native_level(level), "[{}] {}", module, message);
        return {};
    } catch (const spdlog::spdlog_ex& error) {
        return std::unexpected(Error{ErrorCode::io_error,
            "Failed to write log entry", {error.what()}});
    }
}

Result<void> Logger::flush()
{
    try {
        impl_->backend->flush();
        return {};
    } catch (const spdlog::spdlog_ex& error) {
        return std::unexpected(Error{ErrorCode::io_error,
            "Failed to flush logger", {error.what()}});
    }
}

} // namespace dk
