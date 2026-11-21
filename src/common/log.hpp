#pragma once

#include <cstdarg>
#include <mutex>
#include <string>

namespace hyd {

enum class LogLevel : int
{
    trace = 0,
    debug = 1,
    info = 2,
    warn = 3,
    error = 4,
    off = 5,
};

void log_set_level(LogLevel level);
LogLevel log_get_level();
LogLevel log_level_from_string(const std::string& value);
const char* log_level_name(LogLevel level);

void log_message(LogLevel level, const std::string& message);
void log_printf(LogLevel level, const char* fmt, ...) __attribute__((format(printf, 2, 3)));

// Stop writing log lines. Intended for use during shutdown, after the
// logger has reported its final message.
void log_shutdown();

} // namespace hyd

#define HYD_LOG(level, message) ::hyd::log_message(::hyd::LogLevel::level, (message))
#define HYD_LOGF(level, ...) ::hyd::log_printf(::hyd::LogLevel::level, __VA_ARGS__)
