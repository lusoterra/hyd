#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace hyd {

using Bytes = std::vector<std::uint8_t>;
using Hash = std::array<std::uint8_t, 32>;

constexpr std::size_t kHashSize = 32;

std::string hex_encode(std::span<const std::uint8_t> bytes);
std::string hex_encode(const Hash& hash);
std::optional<Bytes> hex_decode(std::string_view text);
std::optional<Hash> hash_from_hex(std::string_view text);

Hash sha256(std::span<const std::uint8_t> bytes);
Hash sha256(std::string_view text);

// zlib (RFC 1950) decompression, streaming. Returns false with an error message
// for malformed input.
bool zlib_inflate(std::span<const std::uint8_t> input, Bytes& output, std::string& error);

std::optional<Bytes> read_file(const std::filesystem::path& path, std::string& error);
bool write_file(const std::filesystem::path& path, std::span<const std::uint8_t> data, std::string& error);
bool write_file_atomic(const std::filesystem::path& path, std::span<const std::uint8_t> data, std::string& error);
bool write_file_atomic(const std::filesystem::path& path, std::string_view data, std::string& error);
bool fsync_path(const std::filesystem::path& path, std::string& error);
bool fsync_directory(const std::filesystem::path& path, std::string& error);
bool mkdir_p(const std::filesystem::path& path, std::string& error);
bool path_exists(const std::filesystem::path& path);
std::uint64_t file_size_or_zero(const std::filesystem::path& path);

Bytes random_bytes(std::size_t count);

std::string trim(std::string_view value);
std::string to_lower(std::string_view value);
std::vector<std::string> split(std::string_view value, char delimiter);
bool starts_with(std::string_view value, std::string_view prefix);
bool ends_with(std::string_view value, std::string_view suffix);

std::optional<std::uint64_t> parse_u64(std::string_view value);
std::optional<std::int64_t> parse_i64(std::string_view value);
std::optional<bool> parse_bool(std::string_view value);

// Percent-decoding for URL query values and form bodies. Invalid escapes are
// preserved literally.
std::string percent_decode(std::string_view value);
std::string percent_encode(std::string_view value);

std::string human_bytes(std::uint64_t bytes);
std::string errno_string(int err);

std::int64_t now_unix();
std::string iso8601_utc(std::int64_t unix_time);

} // namespace hyd
