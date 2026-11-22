#include "common/util.hpp"

#include <openssl/evp.h>
#include <openssl/rand.h>
#include <zlib.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <stdexcept>
#include <fcntl.h>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>

namespace hyd {

std::string hex_encode(std::span<const std::uint8_t> bytes)
{
    static constexpr char digits[] = "0123456789abcdef";
    std::string result;
    result.resize(bytes.size() * 2);
    for (std::size_t i = 0; i < bytes.size(); ++i)
    {
        result[i * 2] = digits[bytes[i] >> 4];
        result[i * 2 + 1] = digits[bytes[i] & 0x0f];
    }
    return result;
}

std::string hex_encode(const Hash& hash)
{
    return hex_encode(std::span<const std::uint8_t>(hash.data(), hash.size()));
}

std::optional<Bytes> hex_decode(std::string_view text)
{
    if (text.size() % 2 != 0)
    {
        return std::nullopt;
    }

    auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };

    Bytes out;
    out.reserve(text.size() / 2);
    for (std::size_t i = 0; i < text.size(); i += 2)
    {
        const int high = nibble(text[i]);
        const int low = nibble(text[i + 1]);
        if (high < 0 || low < 0)
        {
            return std::nullopt;
        }
        out.push_back(static_cast<std::uint8_t>((high << 4) | low));
    }
    return out;
}

std::optional<Hash> hash_from_hex(std::string_view text)
{
    if (text.size() != kHashSize * 2)
    {
        return std::nullopt;
    }

    auto bytes = hex_decode(text);
    if (!bytes || bytes->size() != kHashSize)
    {
        return std::nullopt;
    }

    Hash hash {};
    std::copy(bytes->begin(), bytes->end(), hash.begin());
    return hash;
}

std::optional<Bytes> hash_bytes_from_hex(std::string_view text)
{
    auto bytes = hex_decode(text);
    if (!bytes)
    {
        return std::nullopt;
    }

    if (bytes->size() != 16 && bytes->size() != 20 && bytes->size() != kHashSize)
    {
        return std::nullopt;
    }

    return bytes;
}

namespace {

bool append_access_key(std::string key, std::vector<std::string>& keys, std::string& error)
{
    key = trim(key);
    if (key.empty())
    {
        return true;
    }

    if (!hash_from_hex(key))
    {
        error = "invalid API key: it must be 64 hexadecimal characters";
        return false;
    }

    if (std::find(keys.begin(), keys.end(), key) == keys.end())
    {
        keys.push_back(std::move(key));
    }

    return true;
}

} // namespace

bool parse_key_list(std::string_view text, std::vector<std::string>& keys, std::string& error)
{
    std::string token;
    for (const char character : text)
    {
        if (character == ',' || std::isspace(static_cast<unsigned char>(character)))
        {
            if (!append_access_key(std::move(token), keys, error))
            {
                return false;
            }
            token.clear();
        }
        else
        {
            token.push_back(character);
        }
    }

    return append_access_key(std::move(token), keys, error);
}

bool load_key_file(const std::filesystem::path& path, std::vector<std::string>& keys, std::string& error)
{
    if (!path_exists(path))
    {
        return true;
    }

    auto bytes = read_file(path, error);
    if (!bytes)
    {
        return false;
    }

    const std::string text(bytes->begin(), bytes->end());
    std::size_t line_number = 0;
    std::size_t start = 0;

    while (start <= text.size())
    {
        const std::size_t end = text.find('\n', start);
        const std::string line = text.substr(
            start, end == std::string::npos ? std::string::npos : end - start);
        ++line_number;

        const std::string trimmed = trim(line);
        if (!trimmed.empty() && trimmed.front() != '#')
        {
            if (!hash_from_hex(trimmed))
            {
                error = path.string() + ":" + std::to_string(line_number)
                    + " is not a 64-character hexadecimal API key";
                return false;
            }
            if (std::find(keys.begin(), keys.end(), trimmed) == keys.end())
            {
                keys.push_back(trimmed);
            }
        }

        if (end == std::string::npos)
        {
            break;
        }
        start = end + 1;
    }

    return true;
}

Hash sha256(std::span<const std::uint8_t> bytes)
{
    Hash out {};
    unsigned int length = 0;
    EVP_Digest(bytes.data(), bytes.size(), out.data(), &length, EVP_sha256(), nullptr);
    return out;
}

Hash sha256(std::string_view text)
{
    return sha256(std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(text.data()), text.size()));
}

bool zlib_inflate(std::span<const std::uint8_t> input, Bytes& output, std::string& error)
{
    output.clear();

    // Zip-bomb guard: the largest legitimate update files decompress to tens
    // of MB. Refuse anything beyond 512 MiB instead of OOMing on a malicious
    // mirror.
    constexpr std::size_t kMaxInflatedBytes = 512u * 1024u * 1024u;

    z_stream stream {};
    if (inflateInit(&stream) != Z_OK)
    {
        error = "inflateInit failed";
        return false;
    }

    if (input.size() > 0xffffffffu)
    {
        error = "compressed input is too large";
        inflateEnd(&stream);
        return false;
    }

    stream.next_in = const_cast<Bytef*>(input.data());
    stream.avail_in = static_cast<uInt>(input.size());

    std::array<std::uint8_t, 1 << 16> buffer {};
    int result = Z_OK;

    while (result != Z_STREAM_END)
    {
        stream.next_out = buffer.data();
        stream.avail_out = static_cast<uInt>(buffer.size());

        result = inflate(&stream, Z_NO_FLUSH);
        if (result != Z_OK && result != Z_STREAM_END)
        {
            error = stream.msg != nullptr ? stream.msg : "inflate failed";
            inflateEnd(&stream);
            return false;
        }

        const std::size_t produced = buffer.size() - stream.avail_out;
        if (output.size() + produced > kMaxInflatedBytes)
        {
            error = "inflated output exceeds 512 MiB; refusing a likely zip-bomb";
            inflateEnd(&stream);
            return false;
        }
        output.insert(output.end(), buffer.data(), buffer.data() + produced);

        if (result != Z_STREAM_END && stream.avail_in == 0 && produced == 0)
        {
            error = "truncated zlib stream";
            inflateEnd(&stream);
            return false;
        }
    }

    inflateEnd(&stream);
    return true;
}

bool zlib_deflate(std::span<const std::uint8_t> input, Bytes& output, std::string& error)
{
    if (input.size() > 0xffffffffu)
    {
        error = "input is too large to compress";
        return false;
    }

    uLongf output_size = compressBound(static_cast<uLong>(input.size()));
    output.resize(output_size);

    const int result = compress2(output.data(), &output_size,
        input.data(), static_cast<uLong>(input.size()), Z_DEFAULT_COMPRESSION);
    if (result != Z_OK)
    {
        output.clear();
        error = "zlib compression failed (" + std::to_string(result) + ")";
        return false;
    }

    output.resize(output_size);
    return true;
}

std::optional<Bytes> read_file(const std::filesystem::path& path, std::string& error)
{
    std::ifstream file(path, std::ios::binary);
    if (!file)
    {
        error = "cannot open " + path.string() + ": " + errno_string(errno);
        return std::nullopt;
    }

    file.seekg(0, std::ios::end);
    const std::streamoff size = file.tellg();
    if (size < 0)
    {
        error = "cannot stat " + path.string();
        return std::nullopt;
    }
    file.seekg(0, std::ios::beg);

    Bytes data(static_cast<std::size_t>(size));
    if (size > 0)
    {
        file.read(reinterpret_cast<char*>(data.data()), size);
        if (!file)
        {
            error = "cannot read " + path.string();
            return std::nullopt;
        }
    }

    return data;
}

bool write_file(const std::filesystem::path& path, std::span<const std::uint8_t> data, std::string& error)
{
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file)
    {
        error = "cannot open " + path.string() + ": " + errno_string(errno);
        return false;
    }

    if (!data.empty())
    {
        file.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    }

    if (!file)
    {
        error = "cannot write " + path.string() + ": " + errno_string(errno);
        return false;
    }

    file.flush();
    if (!file)
    {
        error = "cannot flush " + path.string() + ": " + errno_string(errno);
        return false;
    }

    return true;
}

bool write_file_atomic(const std::filesystem::path& path, std::span<const std::uint8_t> data, std::string& error)
{
    const auto parent = path.parent_path();
    if (!parent.empty())
    {
        if (!mkdir_p(parent, error))
        {
            return false;
        }
    }

    // Unique, unpredictable tmp name per call: pid + 64-bit randomness.
    // O_EXCL|O_NOFOLLOW closes the symlink-plant + cross-thread collide
    // window of the old `path.tmp.<pid>` scheme. Retry on EEXIST.
    std::string temporary;
    int fd = -1;
    for (int attempt = 0; attempt < 8; ++attempt)
    {
        const auto nonce = hex_encode(random_bytes(8));
        temporary = path.string() + ".tmp." + std::to_string(::getpid()) + "." + nonce;
        fd = ::open(temporary.c_str(),
            O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644);
        if (fd >= 0)
        {
            break;
        }
        if (errno != EEXIST)
        {
            error = "cannot create " + temporary + ": " + errno_string(errno);
            return false;
        }
    }
    if (fd < 0)
    {
        error = "cannot create temp file for " + path.string() + ": " + errno_string(EEXIST);
        return false;
    }

    {
        // fd already open with O_EXCL|O_NOFOLLOW above.

        std::size_t offset = 0;
        while (offset < data.size())
        {
            const ssize_t written = ::write(fd, data.data() + offset, data.size() - offset);
            if (written < 0)
            {
                if (errno == EINTR)
                {
                    continue;
                }
                error = "cannot write " + temporary + ": " + errno_string(errno);
                ::close(fd);
                ::unlink(temporary.c_str());
                return false;
            }
            offset += static_cast<std::size_t>(written);
        }

        if (::fsync(fd) != 0)
        {
            error = "cannot fsync " + temporary + ": " + errno_string(errno);
            ::close(fd);
            ::unlink(temporary.c_str());
            return false;
        }

        if (::close(fd) != 0)
        {
            error = "cannot close " + temporary + ": " + errno_string(errno);
            ::unlink(temporary.c_str());
            return false;
        }
    }

    if (::rename(temporary.c_str(), path.c_str()) != 0)
    {
        error = "cannot rename " + temporary + " to " + path.string() + ": " + errno_string(errno);
        ::unlink(temporary.c_str());
        return false;
    }

    if (!parent.empty())
    {
        if (!fsync_directory(parent, error))
        {
            return false;
        }
    }

    return true;
}

bool write_file_atomic(const std::filesystem::path& path, std::string_view data, std::string& error)
{
    return write_file_atomic(path,
        std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(data.data()), data.size()),
        error);
}

bool fsync_path(const std::filesystem::path& path, std::string& error)
{
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0)
    {
        error = "cannot open " + path.string() + ": " + errno_string(errno);
        return false;
    }
    if (::fsync(fd) != 0)
    {
        error = "cannot fsync " + path.string() + ": " + errno_string(errno);
        ::close(fd);
        return false;
    }
    ::close(fd);
    return true;
}

bool fsync_directory(const std::filesystem::path& path, std::string& error)
{
    return fsync_path(path, error);
}

bool mkdir_p(const std::filesystem::path& path, std::string& error)
{
    std::error_code ec;
    if (std::filesystem::exists(path, ec))
    {
        if (!std::filesystem::is_directory(path, ec))
        {
            error = path.string() + " exists but is not a directory";
            return false;
        }
        return true;
    }

    if (!std::filesystem::create_directories(path, ec) && ec)
    {
        error = "cannot create " + path.string() + ": " + ec.message();
        return false;
    }
    return true;
}

bool path_exists(const std::filesystem::path& path)
{
    std::error_code ec;
    return std::filesystem::exists(path, ec);
}

std::uint64_t file_size_or_zero(const std::filesystem::path& path)
{
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    return ec ? 0 : static_cast<std::uint64_t>(size);
}

Bytes random_bytes(std::size_t count)
{
    Bytes bytes(count);
    if (count > 0 && RAND_bytes(bytes.data(), static_cast<int>(count)) != 1)
    {
        // RAND_bytes failing means the CSPRNG is broken; throwing is the only
        // safe behaviour.
        throw std::runtime_error("OpenSSL RAND_bytes failed");
    }
    return bytes;
}

std::string trim(std::string_view value)
{
    std::size_t begin = 0;
    std::size_t end = value.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(value[begin]))) ++begin;
    while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1]))) --end;
    return std::string(value.substr(begin, end - begin));
}

std::string to_lower(std::string_view value)
{
    std::string out(value);
    std::transform(out.begin(), out.end(), out.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

std::vector<std::string> split(std::string_view value, char delimiter)
{
    std::vector<std::string> out;
    std::size_t start = 0;
    while (true)
    {
        const std::size_t position = value.find(delimiter, start);
        if (position == std::string_view::npos)
        {
            out.emplace_back(value.substr(start));
            break;
        }
        out.emplace_back(value.substr(start, position - start));
        start = position + 1;
    }
    return out;
}

bool starts_with(std::string_view value, std::string_view prefix)
{
    return value.size() >= prefix.size() && value.substr(0, prefix.size()) == prefix;
}

bool ends_with(std::string_view value, std::string_view suffix)
{
    return value.size() >= suffix.size() && value.substr(value.size() - suffix.size()) == suffix;
}

std::optional<std::uint64_t> parse_u64(std::string_view value)
{
    if (value.empty())
    {
        return std::nullopt;
    }

    std::uint64_t result = 0;
    for (char c : value)
    {
        if (c < '0' || c > '9')
        {
            return std::nullopt;
        }
        const std::uint64_t digit = static_cast<std::uint64_t>(c - '0');
        if (result > (UINT64_MAX - digit) / 10)
        {
            return std::nullopt;
        }
        result = result * 10 + digit;
    }
    return result;
}

std::optional<std::int64_t> parse_i64(std::string_view value)
{
    if (value.empty())
    {
        return std::nullopt;
    }
    bool negative = false;
    if (value.front() == '-')
    {
        negative = true;
        value.remove_prefix(1);
    }
    const auto magnitude = parse_u64(value);
    if (!magnitude || *magnitude > static_cast<std::uint64_t>(INT64_MAX))
    {
        return std::nullopt;
    }
    const auto signed_value = static_cast<std::int64_t>(*magnitude);
    return negative ? -signed_value : signed_value;
}

std::optional<bool> parse_bool(std::string_view value)
{
    const auto lower = to_lower(trim(value));
    if (lower == "1" || lower == "true" || lower == "yes" || lower == "on") return true;
    if (lower == "0" || lower == "false" || lower == "no" || lower == "off") return false;
    return std::nullopt;
}

std::string percent_decode(std::string_view value)
{
    std::string out;
    out.reserve(value.size());

    for (std::size_t i = 0; i < value.size(); ++i)
    {
        if (value[i] == '%' && i + 2 < value.size())
        {
            auto nibble = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return -1;
            };
            const int high = nibble(value[i + 1]);
            const int low = nibble(value[i + 2]);
            if (high >= 0 && low >= 0)
            {
                out.push_back(static_cast<char>((high << 4) | low));
                i += 2;
                continue;
            }
        }
        else if (value[i] == '+')
        {
            out.push_back(' ');
            continue;
        }
        out.push_back(value[i]);
    }

    return out;
}

std::string percent_encode(std::string_view value)
{
    static constexpr char digits[] = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : value)
    {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')
            || c == '-' || c == '_' || c == '.' || c == '~')
        {
            out.push_back(static_cast<char>(c));
        }
        else
        {
            out.push_back('%');
            out.push_back(digits[c >> 4]);
            out.push_back(digits[c & 0x0f]);
        }
    }
    return out;
}

std::string human_bytes(std::uint64_t bytes)
{
    static constexpr const char* units[] = { "B", "KiB", "MiB", "GiB", "TiB", "PiB" };
    double value = static_cast<double>(bytes);
    std::size_t unit = 0;
    while (value >= 1024.0 && unit + 1 < std::size(units))
    {
        value /= 1024.0;
        ++unit;
    }

    std::ostringstream stream;
    if (unit == 0)
    {
        stream << bytes << ' ' << units[unit];
    }
    else
    {
        stream << std::fixed << std::setprecision(2) << value << ' ' << units[unit];
    }
    return stream.str();
}

std::string errno_string(int err)
{
    char buffer[256];
    buffer[0] = '\0';
#if defined(__GLIBC__) && defined(_GNU_SOURCE)
    return std::string(strerror_r(err, buffer, sizeof(buffer)));
#else
    if (strerror_r(err, buffer, sizeof(buffer)) == 0)
    {
        return std::string(buffer);
    }
    return "errno " + std::to_string(err);
#endif
}

std::int64_t now_unix()
{
    return static_cast<std::int64_t>(std::time(nullptr));
}

std::string iso8601_utc(std::int64_t unix_time)
{
    std::time_t time = static_cast<std::time_t>(unix_time);
    std::tm tm {};
    gmtime_r(&time, &tm);
    char buffer[32];
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buffer;
}

} // namespace hyd
