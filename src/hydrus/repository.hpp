#pragma once

#include "common/util.hpp"
#include "hydrus/http.hpp"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace hyd {

struct RepositoryUpdate
{
    std::int64_t index = 0;
    Hash hash {};
    std::int64_t begin = 0;
    std::int64_t end = 0;
};

struct RepositoryMetadata
{
    // index -> updates. Update files with no content are represented by an
    // empty hash list.
    std::map<std::int64_t, std::vector<RepositoryUpdate>> updates;
    std::int64_t next_update_due = 0;

    std::int64_t highest_index() const;
    std::size_t update_file_count() const;
    std::vector<RepositoryUpdate> all_updates_ordered() const;

    void merge(const RepositoryMetadata& other);
};

class RepositoryClient
{
public:
    RepositoryClient(std::string base_url, std::string access_key);

    bool fetch_metadata(std::int64_t since, RepositoryMetadata& metadata, std::string& error);
    bool fetch_update(const Hash& hash, Bytes& compressed_body, std::string& error);

    void set_timeout_seconds(long seconds) { http_.set_timeout_seconds(seconds); }
    void set_rate_limit_kib(std::uint64_t kib) { http_.set_rate_limit_kib(kib); }

private:
    HttpClient http_;
};

bool load_repository_metadata_file(const std::filesystem::path& path, RepositoryMetadata& metadata, std::string& error);
bool save_repository_metadata_file(const std::filesystem::path& path, const RepositoryMetadata& metadata, std::string& error);

bool parse_repository_metadata_bytes(std::span<const std::uint8_t> compressed, RepositoryMetadata& metadata, std::string& error);
bool parse_repository_metadata_json(std::string_view json_text, RepositoryMetadata& metadata, std::string& error);

} // namespace hyd
