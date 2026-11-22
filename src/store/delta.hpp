#pragma once

#include "common/util.hpp"
#include "store/format.hpp"

#include <filesystem>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace hyd {

// In-memory view of all un-compacted incremental syncs. Delta files are
// append-only and small compared to the base store (a weekly PTR sync is on
// the order of a megabyte or two).
struct DeltaData
{
    std::unordered_map<std::uint32_t, std::string> tags;
    std::vector<std::vector<HashEntry>> hashes_by_partition;
    std::vector<LegacyHashEntry> legacy_hashes;

    std::unordered_map<std::uint32_t, std::vector<std::uint32_t>> mapping_additions;
    std::unordered_set<std::uint64_t> mapping_deletions;
    std::unordered_set<std::uint32_t> deleted_files;

    std::unordered_map<std::uint32_t, std::uint32_t> sibling_additions;
    std::unordered_set<std::uint64_t> sibling_deletions;

    std::unordered_map<std::uint32_t, std::vector<std::uint32_t>> parent_additions;
    std::unordered_set<std::uint64_t> parent_deletions;

    std::uint64_t record_count = 0;
    std::uint64_t mapping_add_count = 0;
    std::uint64_t mapping_delete_count = 0;

    void init(std::size_t partitions);
    void absorb(const DeltaData& other);

    std::int64_t approximate_bytes() const;
};

inline std::uint64_t pair_key(std::uint32_t first, std::uint32_t second)
{
    return (static_cast<std::uint64_t>(first) << 32) | second;
}

// Loads every delta-*.bin in the directory, in filename order. Truncated
// tails are tolerated (they are the result of a crash mid-append) and are
// reported through truncated.
bool load_deltas(const std::filesystem::path& directory, std::size_t partitions, DeltaData& data,
    bool& truncated, std::string& error);

} // namespace hyd
