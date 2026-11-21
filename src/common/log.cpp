#include "common/log.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <vector>

namespace hyd {

namespace {

std::atomic<int> g_level { static_cast<int>(LogLevel::info) };
std::mutex g_mutex;
bool g_shutdown = false;

std::string timestamp_now()
{
    using namespace std::chrono;

    const auto now = system_clock::now();
    const auto secs = time_point_cast<seconds>(now);
    const auto ms = duration_cast<milliseconds>(now - secs).count();
    const std::time_t tt = system_clock::to_time_t(secs);

    std::tm tm {};
    gmtime_r(&tt, &tm);

    char buffer[80];
    std::snprintf(buffer, sizeof(buffer), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
        tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
        tm.tm_hour, tm.tm_min, tm.tm_sec, static_cast<int>(ms));
    return buffer;
}

} // namespace

void log_set_level(LogLevel level)
{
    g_level.store(static_cast<int>(level), std::memory_order_relaxed);
}

LogLevel log_get_level()
{
    return static_cast<LogLevel>(g_level.load(std::memory_order_relaxed));
}

LogLevel log_level_from_string(const std::string& value)
{
    if (value == "trace") return LogLevel::trace;
    if (value == "debug") return LogLevel::debug;
    if (value == "info") return LogLevel::info;
    if (value == "warn" || value == "warning") return LogLevel::warn;
    if (value == "error") return LogLevel::error;
    if (value == "off" || value == "none") return LogLevel::off;
    return LogLevel::info;
}

const char* log_level_name(LogLevel level)
{
    switch (level)
    {
        case LogLevel::trace: return "trace";
        case LogLevel::debug: return "debug";
        case LogLevel::info: return "info";
        case LogLevel::warn: return "warn";
        case LogLevel::error: return "error";
        case LogLevel::off: return "off";
    }
    return "?";
}

void log_message(LogLevel level, const std::string& message)
{
    if (static_cast<int>(level) < g_level.load(std::memory_order_relaxed))
    {
        return;
    }

    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_shutdown)
    {
        return;
    }

    std::fprintf(stderr, "%s [%s] %s\n", timestamp_now().c_str(), log_level_name(level), message.c_str());
    std::fflush(stderr);
}

void log_printf(LogLevel level, const char* fmt, ...)
{
    if (static_cast<int>(level) < g_level.load(std::memory_order_relaxed))
    {
        return;
    }

    va_list args;
    va_start(args, fmt);
    va_list copy;
    va_copy(copy, args);
    const int needed = std::vsnprintf(nullptr, 0, fmt, copy);
    va_end(copy);

    if (needed < 0)
    {
        va_end(args);
        return;
    }

    std::vector<char> buffer(static_cast<size_t>(needed) + 1);
    std::vsnprintf(buffer.data(), buffer.size(), fmt, args);
    va_end(args);

    log_message(level, std::string(buffer.data(), static_cast<size_t>(needed)));
}

void log_shutdown()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_shutdown = true;
}

} // namespace hyd
