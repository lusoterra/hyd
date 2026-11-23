#pragma once

#include "common/util.hpp"
#include "store/delta.hpp"
#include "store/format.hpp"
#include "store/tag_source.hpp"

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace hyd {

struct StoreManifest
{
    std::uint32_t format = kStoreFormatVersion;
    std::uint32_t partitions = 16;
    // Bumped every time the base files are rewritten (initial build or
    // compaction). Derived files (counts, tag names, postings) carry this and
    // are ignored when it does not match, so a crash between the manifest and
    // a derived file can never serve stale metadata.
    std::uint64_t generation = 0;
    std::uint64_t hash_count = 0;
    std::uint64_t mapping_count = 0;
    std::uint64_t tag_count = 0;
    std::int64_t built_at = 0;

    struct TagSegmentInfo
    {
        std::uint64_t base = 0;
        std::uint64_t count = 0;
        std::uint64_t last = 0;
        std::string idx_path;
        std::string str_path;
        std::string ids_path;
    };

    std::vector<TagSegmentInfo> tag_segments;
};

bool load_store_manifest(const std::filesystem::path& store_dir, StoreManifest& manifest, std::string& error);
bool save_store_manifest(const std::filesystem::path& store_dir, const StoreManifest& manifest, std::string& error);

class Store : public TagSource
{
public:
    Store();
    ~Store();

    Store(const Store&) = delete;
    Store& operator=(const Store&) = delete;

    bool open(const std::filesystem::path& store_dir, std::string& error);
    bool is_open() const { return open_; }

    // Thread-safe. Returns found=false for hashes the repository has never
    // seen (or has deleted). Accepts SHA-256, SHA-1, and MD5 hash bytes.
    TagQueryResult lookup(std::span<const std::uint8_t> hash) const override;

    // Current mapping count for an exact storage tag name, including the
    // pending delta. Returns nullopt when the tag is not defined or when the
    // derived count files have not been built.
    std::optional<std::uint64_t> tag_count(std::string_view tag) const override;
    bool has_tag_counts() const override;
    std::int64_t tag_counts_built_at() const override;

    bool has_tag_hashes() const override;
    std::optional<TagHashesPage> tag_hashes(std::string_view tag,
        std::uint64_t offset, std::uint32_t limit,
        std::optional<std::uint32_t> after = std::nullopt) const override;

    std::optional<std::string> tag_string(std::uint32_t tag_id) const;

    const StoreManifest& manifest() const { return manifest_; }
    const DeltaData& delta() const { return delta_; }
    std::uint64_t delta_record_count() const override { return delta_.record_count; }

    std::size_t partitions() const { return manifest_.partitions; }
    std::uint32_t partition_for(const Hash& hash) const;
    std::uint32_t partition_for(std::span<const std::uint8_t> hash) const;

    // Exposed for diagnostics.
    std::uint64_t stored_hash_count() const override { return manifest_.hash_count; }
    std::uint64_t stored_mapping_count() const override { return manifest_.mapping_count; }
    std::uint64_t stored_tag_count() const override { return manifest_.tag_count; }

    // Base store plus pending delta, as best as can be calculated without
    // walking every partition. Re-applied deltas may make these estimates
    // slightly conservative.
    std::uint64_t total_hash_count() const override;
    std::uint64_t total_mapping_count() const override;
    std::uint64_t total_tag_count() const override;
    std::uint64_t total_sibling_count() const override;
    std::uint64_t total_parent_count() const override;
    std::uint64_t legacy_hash_count() const override;
    std::size_t partition_count() const override { return manifest_.partitions; }
    std::string backend_name() const override { return "custom"; }
    std::uint64_t delta_hash_count() const override;

private:
    struct Impl;

    std::optional<std::uint32_t> resolve_tag_id(std::string_view tag) const;

    Impl* impl_;
    StoreManifest manifest_;
    DeltaData delta_;
    bool open_ = false;
};

} // namespace hyd
