#pragma once

#include "store/tag_source.hpp"

#include <filesystem>
#include <string>

namespace hyd {

// Read-only view of the plain SQLite mirror produced when
// compression = false.
class SqliteStore : public TagSource
{
public:
    SqliteStore();
    ~SqliteStore();

    SqliteStore(const SqliteStore&) = delete;
    SqliteStore& operator=(const SqliteStore&) = delete;

    bool open(const std::filesystem::path& path, std::string& error);

    TagQueryResult lookup(const Hash& hash) const override;

    std::uint64_t total_hash_count() const override;
    std::uint64_t total_mapping_count() const override;
    std::uint64_t total_tag_count() const override;

    std::string backend_name() const override { return "sqlite"; }

private:
    struct Impl;
    Impl* impl_;
    bool open_ = false;
};

} // namespace hyd
