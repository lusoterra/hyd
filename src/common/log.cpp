#include "common/log.hpp"

#include <zlib.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <mutex>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace hyd {

namespace {

std::atomic<int> g_level { static_cast<int>(LogLevel::info) };
std::mutex g_mutex;
bool g_shutdown = false;

int g_file_fd = -1;
std::string g_file_path;
std::uint64_t g_file_bytes = 0;
std::uint64_t g_rotate_bytes = 0;
int g_rotate_keep = 5;
bool g_rotate_compress = true;

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

bool gzip_file(const std::string& source, const std::string& destination)
{
    std::FILE* input = std::fopen(source.c_str(), "rb");
    if (input == nullptr)
    {
        return false;
    }

    gzFile output = gzopen(destination.c_str(), "wb");
    if (output == nullptr)
    {
        std::fclose(input);
        return false;
    }

    std::array<char, 1 << 16> buffer {};
    bool ok = true;
    while (true)
    {
        const std::size_t read = std::fread(buffer.data(), 1, buffer.size(), input);
        if (read > 0 && gzwrite(output, buffer.data(), static_cast<unsigned int>(read)) != static_cast<int>(read))
        {
            ok = false;
            break;
        }
        if (read < buffer.size())
        {
            if (std::ferror(input) != 0)
            {
                ok = false;
            }
            break;
        }
    }

    std::fclose(input);
    if (gzclose(output) != Z_OK)
    {
        ok = false;
    }

    if (!ok)
    {
        std::error_code ec;
        std::filesystem::remove(destination, ec);
    }

    return ok;
}

void remove_if_exists(const std::string& path)
{
    std::error_code ec;
    std::filesystem::remove(path, ec);
}

// Called with g_mutex held. Reopens g_file_fd even if rotation fails, so that
// logging keeps working after a transient filesystem problem.
void rotate_locked()
{
    if (g_file_fd >= 0)
    {
        ::close(g_file_fd);
        g_file_fd = -1;
    }

    const std::string suffix = g_rotate_compress ? ".gz" : "";
    const std::string current = g_file_path;
    const std::string first = g_file_path + ".1" + suffix;

    remove_if_exists(g_file_path + "." + std::to_string(g_rotate_keep) + suffix);

    for (int i = g_rotate_keep; i >= 2; --i)
    {
        const std::string from = g_file_path + "." + std::to_string(i - 1) + suffix;
        const std::string to = g_file_path + "." + std::to_string(i) + suffix;
        remove_if_exists(to);
        std::error_code ec;
        std::filesystem::rename(from, to, ec);
    }

    bool rotated = false;
    if (g_rotate_compress)
    {
        rotated = gzip_file(current, first);
        if (rotated)
        {
            remove_if_exists(current);
        }
    }

    if (!rotated)
    {
        std::error_code ec;
        std::filesystem::rename(current, first, ec);
        rotated = !ec;
    }

    if (!rotated)
    {
        std::fprintf(stderr, "hydd: WARNING: could not rotate log file %s\n", current.c_str());
    }

    g_file_fd = ::open(current.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0640);
    if (g_file_fd >= 0)
    {
        ::fchmod(g_file_fd, 0640);
        struct stat info {};
        if (::fstat(g_file_fd, &info) == 0)
        {
            g_file_bytes = static_cast<std::uint64_t>(info.st_size);
        }
        else
        {
            g_file_bytes = 0;
        }
    }
    else
    {
        g_file_bytes = 0;
        std::fprintf(stderr, "hydd: WARNING: cannot reopen log file %s: %s\n",
            current.c_str(), std::strerror(errno));
    }
}

void write_file_locked(const std::string& line)
{
    if (g_file_fd < 0)
    {
        return;
    }

    if (g_rotate_bytes > 0 && g_file_bytes > 0 && g_file_bytes + line.size() > g_rotate_bytes)
    {
        rotate_locked();
        if (g_file_fd < 0)
        {
            return;
        }
    }

    std::size_t offset = 0;
    while (offset < line.size())
    {
        const ssize_t written = ::write(g_file_fd, line.data() + offset, line.size() - offset);
        if (written < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            std::fprintf(stderr, "hydd: WARNING: log write failed: %s\n", std::strerror(errno));
            ::close(g_file_fd);
            g_file_fd = -1;
            return;
        }
        offset += static_cast<std::size_t>(written);
    }

    g_file_bytes += line.size();
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

bool log_init_file(const std::string& path, std::uint64_t rotate_bytes, int rotate_keep,
    bool rotate_compress, std::string& error)
{
    std::lock_guard<std::mutex> lock(g_mutex);

    if (g_file_fd >= 0)
    {
        ::close(g_file_fd);
        g_file_fd = -1;
    }

    g_file_path = path;
    g_rotate_bytes = rotate_bytes;
    g_rotate_keep = rotate_keep > 0 ? rotate_keep : 0;
    g_rotate_compress = rotate_compress;
    g_file_bytes = 0;

    if (path.empty())
    {
        return true;
    }

    std::error_code ec;
    const auto parent = std::filesystem::path(path).parent_path();
    if (!parent.empty() && !std::filesystem::exists(parent, ec))
    {
        std::filesystem::create_directories(parent, ec);
        if (ec)
        {
            error = "cannot create log directory " + parent.string() + ": " + ec.message();
            return false;
        }
    }

    g_file_fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0640);
    if (g_file_fd < 0)
    {
        error = "cannot open log file " + path + ": " + std::strerror(errno);
        return false;
    }

    ::fchmod(g_file_fd, 0640);
    struct stat info {};
    if (::fstat(g_file_fd, &info) == 0)
    {
        g_file_bytes = static_cast<std::uint64_t>(info.st_size);
    }

    return true;
}

void log_close_file()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_file_fd >= 0)
    {
        ::close(g_file_fd);
        g_file_fd = -1;
    }
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

    const std::string line = timestamp_now() + " [" + log_level_name(level) + "] " + message + "\n";
    std::fwrite(line.data(), 1, line.size(), stderr);
    std::fflush(stderr);
    write_file_locked(line);
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
    if (g_file_fd >= 0)
    {
        ::close(g_file_fd);
        g_file_fd = -1;
    }
    g_shutdown = true;
}

} // namespace hyd
