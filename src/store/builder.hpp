#pragma once

#include "common/config.hpp"
#include "hydrus/repository.hpp"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

namespace hyd {

struct SyncState
{
    std::int64_t next_index = 0;
    bool built = false;
    bool partial = false;
    std::int64_t built_at = 0;
    std::int64_t last_sync_at = 0;
    std::int64_t last_sync_started = 0;
    std::uint64_t downloaded_bytes = 0;
    std::uint64_t downloaded_files = 0;
    std::uint32_t delta_generation = 0;
    std::string backend; // "custom" or "sqlite"
    std::uint64_t hash_count = 0;
    std::uint64_t mapping_count = 0;
    std::uint64_t tag_count = 0;
    std::int64_t last_compacted_at = 0;
};

bool load_sync_state(const std::filesystem::path& path, SyncState& state, std::string& error);
bool save_sync_state(const std::filesystem::path& path, const SyncState& state, std::string& error);

struct SyncCallbacks
{
    // Called after the on-disk store has changed and can be re-opened.
    std::function<void()> on_store_ready;
    // Return true to abort the sync promptly.
    std::function<bool()> should_stop;
};

// Performs one sync pass: fetches new metadata, downloads missing update
// files, and either builds the initial store or appends a delta. Returns
// false on error. A stop request is reported through stopped rather than as
// an error.
bool sync_repository(const Config& config, const SyncCallbacks& callbacks, bool& stopped, std::string& error);

// Merges every pending delta file into the immutable base store. This keeps
// startup memory and query overhead flat and reclaims delta space. It never
// discards live tags or mappings. Crash-safe: if interrupted, the deltas are
// simply reapplied on the next run.
bool compact_store(const Config& config, const SyncCallbacks& callbacks, SyncState& state, std::string& error);

// SQLite backend: vacuums the database (reclaims free pages).
bool sqlite_vacuum(const Config& config, std::string& error);

// Checks the configured backend against what is on disk and performs a
// resumable SQLite -> custom migration when compression was turned on.
// Refuses the custom -> SQLite direction loudly.
bool prepare_storage_backend(const Config& config, const SyncCallbacks& callbacks, SyncState& state,
    bool& stopped, std::string& error);

bool migrate_sqlite_to_custom(const Config& config, const SyncCallbacks& callbacks, SyncState& state,
    bool& stopped, std::string& error);

std::uint64_t delta_bytes_on_disk(const Config& config);

// Removes downloaded update files that are no longer needed.
bool clean_update_cache(const Config& config, std::string& error);

} // namespace hyd
