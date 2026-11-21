#pragma once

#include <cstdarg>
#include <cstdint>
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

// Opens (or creates) a log file. rotate_bytes == 0 disables rotation.
// Compression is gzip. Returns false and sets error when the file cannot be
// opened; logging to stderr continues either way.
bool log_init_file(const std::string& path, std::uint64_t rotate_bytes, int rotate_keep,
    bool rotate_compress, std::string& error);
void log_close_file();

void log_message(LogLevel level, const std::string& message);
void log_printf(LogLevel level, const char* fmt, ...) __attribute__((format(printf, 2, 3)));

// Stop writing log lines. Intended for use during shutdown, after the
// logger has reported its final message.
void log_shutdown();

} // namespace hyd

#define HYD_LOG(level, message) ::hyd::log_message(::hyd::LogLevel::level, (message))
#define HYD_LOGF(level, ...) ::hyd::log_printf(::hyd::LogLevel::level, __VA_ARGS__)
