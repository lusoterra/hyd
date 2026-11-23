#pragma once

#include "common/config.hpp"
#include "hydrus/repository.hpp"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace hyd {

struct SyncState
{
    std::int64_t next_index = 0;
    bool built = false;
    bool partial = false;
    std::int64_t built_at = 0;
    std::int64_t last_sync_at = 0;
    std::int64_t last_sync_started = 0;
    std::int64_t next_update_due = 0;
    std::uint64_t downloaded_bytes = 0;
    std::uint64_t downloaded_files = 0;
    std::uint32_t delta_generation = 0;
    std::uint64_t hash_count = 0;
    std::uint64_t mapping_count = 0;
    std::uint64_t tag_count = 0;
    std::int64_t last_compacted_at = 0;
};

bool load_sync_state(const std::filesystem::path& path, SyncState& state, std::string& error);
bool save_sync_state(const std::filesystem::path& path, const SyncState& state, std::string& error);

// Live progress for the one-time, multi-pass initial build. Only the initial
// build fills this in; incremental syncs and compaction are quick enough that
// the normal sync logs suffice.
struct BuildProgress
{
    enum Phase : int
    {
        phase_idle = 0,
        phase_definitions = 1,
        phase_partitions = 2,
    };

    std::atomic<int> phase { phase_idle };
    std::atomic<int> partition { 0 };
    std::atomic<int> partitions { 0 };
    std::atomic<std::uint64_t> files_done { 0 };
    std::atomic<std::uint64_t> files_total { 0 };
    std::atomic<std::uint64_t> bytes_done { 0 };
    std::atomic<std::uint64_t> bytes_total { 0 };
    std::atomic<std::uint64_t> hashes { 0 };
    std::atomic<std::uint64_t> mappings { 0 };
    std::atomic<std::int64_t> started_at { 0 };
    std::atomic<std::int64_t> partition_started_at { 0 };
    std::atomic<std::int64_t> updated_at { 0 };
};

struct SyncCallbacks
{
    // Called after the on-disk store has changed and can be re-opened.
    std::function<void()> on_store_ready;
    // Return true to abort the sync promptly.
    std::function<bool()> should_stop;
    // Optional live progress for the initial multi-pass build.
    std::shared_ptr<BuildProgress> progress;
};

// Performs one sync pass: fetches new metadata, downloads missing update
// files, and either builds the initial store or appends a delta. Returns
// false on error. A stop request is reported through stopped rather than as
// an error.
//
// If an initial-build checkpoint matches the persisted repository snapshot,
// this pass finishes that snapshot without first fetching newer indices. This
// keeps a long initial build resumable even when the PTR publishes while the
// daemon is down or the build is running. New indices are fetched as a normal
// incremental delta after the snapshot is complete (or on the next pass if the
// repository is temporarily unreachable).
bool sync_repository(const Config& config, const SyncCallbacks& callbacks, bool& stopped, std::string& error);

// Merges every pending delta file into the immutable base store. This keeps
// startup memory and query overhead flat and reclaims delta space. It never
// discards live tags or mappings. Crash-safe: if interrupted, the deltas are
// simply reapplied on the next run.
bool compact_store(const Config& config, const SyncCallbacks& callbacks, SyncState& state, std::string& error);

// Rebuilds the derived tag-count and tag-name lookup files from the base
// store. Used on upgrade from a store built before those files existed, and
// by --rebuild-tag-counts. Never modifies the base store or the delta journal.
bool rebuild_tag_metadata(const Config& config, const SyncCallbacks& callbacks, std::string& error);

struct TagMetadataCheck
{
    std::uint64_t tag_ids = 0;
    std::uint64_t checked = 0;
    std::uint64_t mismatches = 0;
    std::uint64_t lookup_entries = 0;
    std::uint64_t expected_lookup_entries = 0;
};

// Recomputes the base counts and compares them with counts.bin without writing
// anything. Used by --check-tag-counts.
bool check_tag_metadata(const Config& config, const SyncCallbacks& callbacks,
    TagMetadataCheck& result, std::string& error);

// Ensures sync state exists with defaults on startup. The custom store is the
// only backend, and legacy directories are intentionally ignored.
bool validate_storage_backend(const Config& config, SyncState& state, std::string& error);

std::uint64_t delta_bytes_on_disk(const Config& config);

// Removes downloaded update files that are no longer needed.
bool clean_update_cache(const Config& config, std::string& error);

struct CacheRepairResult
{
    std::uint64_t checked = 0;
    std::uint64_t missing = 0;
    std::uint64_t corrupt = 0;
    std::uint64_t repaired = 0;
    std::uint64_t failed = 0;
};

// Verifies every update file referenced by state/repository.json and
// re-downloads missing or corrupt files. This never touches the query store.
// Requires keep_update_cache = true.
bool repair_update_cache(const Config& config, const SyncCallbacks& callbacks,
    CacheRepairResult& result, std::string& error);

// Repairs only the given update hashes. Used by the running daemon when
// /update notices that a cached file no longer matches its name.
bool repair_update_files(const Config& config, const std::vector<Hash>& hashes,
    const SyncCallbacks& callbacks, CacheRepairResult& result, std::string& error);

} // namespace hyd
