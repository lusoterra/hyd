#pragma once

#include "common/util.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
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

struct TagHashesPage
{
    std::uint64_t total = 0;
    std::uint64_t offset = 0;
    bool has_more = false;
    std::uint64_t next_offset = 0;
    std::optional<std::uint32_t> next_after;
    // True when `total` is exact. Multi-tag intersections do not compute a
    // total unless it is the cheap single-tag case.
    bool total_known = true;
    std::vector<std::string> hashes;
};

struct TagHashesQuery
{
    std::vector<std::string> include; // AND
    std::vector<std::string> exclude; // AND NOT
};

// Common read-only interface for the custom store.
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

    // Derived tag metadata. The default implementation reports "not built" so
    // callers can answer 503 instead of inventing a count.
    virtual std::optional<std::uint64_t> tag_count(std::string_view tag) const
    {
        (void)tag;
        return std::nullopt;
    }
    virtual bool has_tag_counts() const { return false; }
    virtual std::int64_t tag_counts_built_at() const { return 0; }
    virtual std::uint64_t tag_counts_defined_tags() const { return 0; }
    virtual std::uint64_t tag_counts_size_bytes() const { return 0; }
    virtual bool tag_counts_delta_applied() const { return false; }
    virtual void compute_count_overlay() const {}

    // Reverse lookup (tag -> hashes). Off by default because the postings
    // index is roughly one u32 per mapping plus a hash-id map.
    virtual bool has_tag_hashes() const { return false; }
    virtual std::int64_t tag_hashes_built_at() const { return 0; }
    virtual std::uint64_t tag_hashes_size_bytes() const { return 0; }
    virtual std::optional<TagHashesPage> tag_hashes(const TagHashesQuery& query,
        std::uint64_t offset, std::uint32_t limit,
        std::optional<std::uint32_t> after = std::nullopt) const
    {
        (void)query;
        (void)offset;
        (void)limit;
        (void)after;
        return std::nullopt;
    }
};

} // namespace hyd
