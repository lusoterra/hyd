#pragma once

#include "common/util.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace hyd {

struct TagQueryResult
{
    bool found = false;
    bool deleted = false;
    std::uint32_t hash_id = 0;
    std::vector<std::string> storage_tags;
    std::vector<std::string> display_tags;
};

// Common read-only interface for the compressed custom store and the plain
// SQLite backend.
class TagSource
{
public:
    virtual ~TagSource() = default;

    virtual TagQueryResult lookup(std::span<const std::uint8_t> hash) const = 0;

    virtual std::uint64_t total_hash_count() const = 0;
    virtual std::uint64_t total_mapping_count() const = 0;
    virtual std::uint64_t total_tag_count() const = 0;

    virtual std::string backend_name() const = 0;

    virtual std::uint64_t stored_hash_count() const { return total_hash_count(); }
    virtual std::uint64_t stored_mapping_count() const { return total_mapping_count(); }
    virtual std::uint64_t stored_tag_count() const { return total_tag_count(); }
    virtual std::uint64_t delta_record_count() const { return 0; }
    virtual std::uint64_t delta_hash_count() const { return 0; }
    virtual std::uint64_t total_sibling_count() const { return 0; }
    virtual std::uint64_t total_parent_count() const { return 0; }
    virtual std::uint64_t legacy_hash_count() const { return 0; }
    virtual std::size_t partition_count() const { return 0; }
};

} // namespace hyd
