#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>

namespace hyd {

class Config
{
public:
    static constexpr const char* kDefaultPath = "/etc/hydd.conf";

    // Loads the file if it exists. A missing default config is not an error;
    // a missing explicitly requested config is.
    bool load(const std::filesystem::path& path, bool required, std::string& error);

    void apply_command_line_overrides(
        const std::optional<std::string>& data_dir,
        const std::optional<std::string>& listen_address,
        const std::optional<int>& listen_port,
        const std::optional<bool>& sync_enabled,
        const std::optional<std::string>& log_level);

    // Resolves paths and fills in derived defaults. Call after load().
    bool finalize(std::string& error);

    std::string get_string(const std::string& key, const std::string& fallback = {}) const;
    std::int64_t get_int(const std::string& key, std::int64_t fallback) const;
    bool get_bool(const std::string& key, bool fallback) const;

    const std::filesystem::path& path() const { return path_; }
    const std::filesystem::path& data_dir() const { return data_dir_; }
    const std::filesystem::path& store_dir() const { return store_dir_; }
    const std::filesystem::path& cache_dir() const { return cache_dir_; }
    const std::filesystem::path& state_dir() const { return state_dir_; }

    std::string listen_address() const { return get_string("listen_address", "127.0.0.1"); }
    int listen_port() const;
    std::string repository_url() const;
    std::string repository_access_key() const;
    std::int64_t sync_interval_seconds() const;
    int partitions() const;
    std::uint64_t max_download_bytes() const;

    // Prints every effective setting with secrets redacted.
    void dump_effective() const;

private:
    std::filesystem::path path_;
    std::filesystem::path data_dir_;
    std::filesystem::path store_dir_;
    std::filesystem::path cache_dir_;
    std::filesystem::path state_dir_;
    std::map<std::string, std::string> values_;
};

} // namespace hyd
