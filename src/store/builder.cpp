#include "store/builder.hpp"

#include "common/log.hpp"
#include "common/util.hpp"
#include "hydrus/downloader.hpp"
#include "hydrus/update.hpp"
#include "store/format.hpp"
#include "store/store.hpp"
#include "store/delta.hpp"

#include <nlohmann/json.hpp>
#include <zlib.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <fstream>
#include <numeric>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace hyd {

namespace {

constexpr const char* kStorePhase = "build";

struct CachePaths
{
    std::filesystem::path root;
    std::filesystem::path updates;
    std::filesystem::path incoming;
};

CachePaths cache_paths(const Config& config)
{
    CachePaths paths;
    paths.root = config.cache_dir();
    paths.updates = paths.root / "updates";
    paths.incoming = paths.root / "incoming";
    return paths;
}

std::filesystem::path update_cache_path(const CachePaths& paths, const Hash& hash)
{
    return paths.updates / (hex_encode(hash) + ".bin");
}

struct UpdateSource
{
    RepositoryUpdate update;
    std::filesystem::path cache_path;
};

bool stopped(const SyncCallbacks& callbacks)
{
    return callbacks.should_stop && callbacks.should_stop();
}

std::string format_eta(std::int64_t seconds)
{
    if (seconds < 0)
    {
        return "unknown";
    }
    if (seconds == 0)
    {
        return "0s";
    }

    char buffer[32];
    if (seconds < 60)
    {
        std::snprintf(buffer, sizeof(buffer), "%llds", static_cast<long long>(seconds));
    }
    else if (seconds < 3600)
    {
        std::snprintf(buffer, sizeof(buffer), "%lldm", static_cast<long long>(seconds / 60));
    }
    else
    {
        std::snprintf(buffer, sizeof(buffer), "%lldh%02lldm",
            static_cast<long long>(seconds / 3600),
            static_cast<long long>((seconds % 3600) / 60));
    }
    return buffer;
}

double progress_percent(std::uint64_t done, std::uint64_t total)
{
    return total == 0 ? 0.0 : 100.0 * static_cast<double>(done) / static_cast<double>(total);
}

struct BuildProgressGuard
{
    BuildProgress* progress = nullptr;

    ~BuildProgressGuard()
    {
        if (progress != nullptr)
        {
            progress->phase.store(BuildProgress::phase_idle);
            progress->updated_at.store(now_unix());
        }
    }
};

struct TagSegmentWriter
{
    struct Entry
    {
        std::uint64_t offset = 0;
        std::uint32_t id = 0;
        std::uint32_t length = 0;
    };

    std::ofstream strings;
    std::filesystem::path strings_path;
    std::filesystem::path offsets_path;
    std::filesystem::path ids_path;
    std::vector<Entry> entries;
    std::uint64_t count = 0;
    std::uint64_t base_id = 0;
    std::uint64_t last_id = 0;
    std::uint64_t offset = 0;
    std::string error;

    bool open(const std::filesystem::path& directory, std::uint64_t generation)
    {
        char name[64];
        std::snprintf(name, sizeof(name), "seg-%06llu", static_cast<unsigned long long>(generation));

        strings_path = directory / (std::string(name) + ".str");
        offsets_path = directory / (std::string(name) + ".idx");
        ids_path = directory / (std::string(name) + ".ids");

        strings.open(strings_path, std::ios::binary | std::ios::trunc);
        if (!strings)
        {
            error = "cannot open tag segment file in " + directory.string();
            return false;
        }

        return true;
    }

    bool add(std::uint32_t tag_id, std::string_view text)
    {
        if (text.size() > 0xffffffffu)
        {
            error = "tag is too long";
            return false;
        }

        Entry entry;
        entry.offset = offset;
        entry.id = tag_id;
        entry.length = static_cast<std::uint32_t>(text.size());
        entries.push_back(entry);

        strings.write(text.data(), static_cast<std::streamsize>(text.size()));
        if (!strings)
        {
            error = "cannot write tag string";
            return false;
        }

        offset += text.size();
        return true;
    }

    bool finish(std::string& out_error)
    {
        strings.flush();
        if (!strings)
        {
            out_error = "cannot flush tag strings";
            return false;
        }
        strings.close();

        std::stable_sort(entries.begin(), entries.end(),
            [](const Entry& left, const Entry& right) { return left.id < right.id; });

        // Keep the newest definition when an id appears more than once.
        std::vector<Entry> unique;
        unique.reserve(entries.size());
        for (std::size_t i = 0; i < entries.size(); ++i)
        {
            if (!unique.empty() && unique.back().id == entries[i].id)
            {
                unique.back() = entries[i];
            }
            else
            {
                unique.push_back(entries[i]);
            }
        }

        std::ofstream offsets(offsets_path, std::ios::binary | std::ios::trunc);
        std::ofstream ids(ids_path, std::ios::binary | std::ios::trunc);
        if (!offsets || !ids)
        {
            out_error = "cannot create tag segment index";
            return false;
        }

        for (const auto& entry : unique)
        {
            ids.write(reinterpret_cast<const char*>(&entry.id), sizeof(entry.id));
            offsets.write(reinterpret_cast<const char*>(&entry.offset), sizeof(entry.offset));
            offsets.write(reinterpret_cast<const char*>(&entry.length), sizeof(entry.length));
        }

        offsets.flush();
        ids.flush();
        if (!offsets || !ids)
        {
            out_error = "cannot write tag segment index";
            return false;
        }
        offsets.close();
        ids.close();

        if (!unique.empty())
        {
            base_id = unique.front().id;
            last_id = unique.back().id;
        }
        count = unique.size();

        if (!fsync_path(strings_path, out_error) || !fsync_path(offsets_path, out_error)
            || !fsync_path(ids_path, out_error))
        {
            return false;
        }

        entries.clear();
        entries.shrink_to_fit();
        return true;
    }
};

bool write_partition_hashes(const std::filesystem::path& path, std::vector<HashEntry>& entries,
    std::string& error)
{
    std::sort(entries.begin(), entries.end(), hash_entry_less);

    // Duplicate hash definitions should not happen; if they do, keep one.
    entries.erase(std::unique(entries.begin(), entries.end(),
                      [](const HashEntry& left, const HashEntry& right) { return left.hash == right.hash; }),
        entries.end());

    Bytes data;
    data.resize(12 + entries.size() * sizeof(HashEntry));
    std::memcpy(data.data(), kHashesMagic, 8);
    write_u32(data.data() + 8, static_cast<std::uint32_t>(entries.size()));
    if (!entries.empty())
    {
        std::memcpy(data.data() + 12, entries.data(), entries.size() * sizeof(HashEntry));
    }

    return write_file_atomic(path, data, error);
}

bool write_legacy_hashes(const std::filesystem::path& path, std::vector<LegacyHashEntry>& entries,
    std::string& error)
{
    std::sort(entries.begin(), entries.end(),
        [](const LegacyHashEntry& left, const LegacyHashEntry& right) {
            return left.hash_id < right.hash_id;
        });

    // A hash id is defined once. If a delta repeats a definition, keep the
    // first copy so lookups stay stable.
    entries.erase(std::unique(entries.begin(), entries.end(),
                      [](const LegacyHashEntry& left, const LegacyHashEntry& right) {
                          return left.hash_id == right.hash_id;
                      }),
        entries.end());
    std::sort(entries.begin(), entries.end(), legacy_hash_entry_less);

    Bytes data;
    data.resize(12 + entries.size() * sizeof(LegacyHashEntry));
    std::memcpy(data.data(), kLegacyMagic, 8);
    write_u32(data.data() + 8, static_cast<std::uint32_t>(entries.size()));
    if (!entries.empty())
    {
        std::memcpy(data.data() + 12, entries.data(), entries.size() * sizeof(LegacyHashEntry));
    }

    return write_file_atomic(path, data, error);
}

bool read_legacy_hashes(const std::filesystem::path& path, std::vector<LegacyHashEntry>& entries,
    std::string& error)
{
    entries.clear();

    if (!path_exists(path))
    {
        // Format 1 stores (built before legacy hash support) have no file.
        return true;
    }

    auto bytes = read_file(path, error);
    if (!bytes)
    {
        return false;
    }

    if (bytes->size() < 12 || std::memcmp(bytes->data(), kLegacyMagic, 8) != 0)
    {
        error = "bad legacy hash index magic in " + path.string();
        return false;
    }

    const auto count = read_u32(bytes->data() + 8);
    if (bytes->size() < 12 + static_cast<std::size_t>(count) * sizeof(LegacyHashEntry))
    {
        error = "legacy hash index is truncated in " + path.string();
        return false;
    }

    entries.resize(count);
    if (count > 0)
    {
        std::memcpy(entries.data(), bytes->data() + 12,
            static_cast<std::size_t>(count) * sizeof(LegacyHashEntry));
    }

    return true;
}

struct MappingRow
{
    std::uint32_t hash_id = 0;
    std::uint32_t offset = 0;
    std::uint32_t count = 0;
};

bool write_partition_maps(const std::filesystem::path& path,
    const std::unordered_map<std::uint32_t, std::vector<std::uint32_t>>& mappings, std::string& error)
{
    std::vector<std::uint32_t> hash_ids;
    hash_ids.reserve(mappings.size());
    for (const auto& [hash_id, tags] : mappings)
    {
        (void)tags;
        hash_ids.push_back(hash_id);
    }
    std::sort(hash_ids.begin(), hash_ids.end());

    std::vector<std::uint32_t> tag_ids;
    tag_ids.reserve(mappings.size() * 8);
    std::vector<std::uint64_t> offsets;
    offsets.reserve(hash_ids.size() + 1);

    for (const auto hash_id : hash_ids)
    {
        const auto iterator = mappings.find(hash_id);
        offsets.push_back(tag_ids.size());
        if (iterator != mappings.end())
        {
            tag_ids.insert(tag_ids.end(), iterator->second.begin(), iterator->second.end());
        }
    }
    offsets.push_back(tag_ids.size());

    Bytes data;
    const std::size_t header = 20;
    const std::size_t ids_size = hash_ids.size() * 4;
    const std::size_t offsets_size = offsets.size() * 8;
    const std::size_t tags_size = tag_ids.size() * 4;
    data.resize(header + ids_size + offsets_size + tags_size);

    std::memcpy(data.data(), kMapsMagic, 8);
    write_u32(data.data() + 8, static_cast<std::uint32_t>(hash_ids.size()));
    write_u32(data.data() + 12, static_cast<std::uint32_t>(tag_ids.size()));
    write_u32(data.data() + 16, 0);

    if (ids_size > 0)
    {
        std::memcpy(data.data() + header, hash_ids.data(), ids_size);
    }
    if (offsets_size > 0)
    {
        std::memcpy(data.data() + header + ids_size, offsets.data(), offsets_size);
    }
    if (tags_size > 0)
    {
        std::memcpy(data.data() + header + ids_size + offsets_size, tag_ids.data(), tags_size);
    }

    return write_file_atomic(path, data, error);
}

bool write_pairs(const std::filesystem::path& path,
    const std::unordered_map<std::uint32_t, std::vector<std::uint32_t>>& parents, std::string& error)
{
    std::vector<std::pair<std::uint32_t, std::uint32_t>> rows;
    for (const auto& [child, ancestors] : parents)
    {
        for (const auto parent : ancestors)
        {
            rows.emplace_back(child, parent);
        }
    }
    std::sort(rows.begin(), rows.end());
    rows.erase(std::unique(rows.begin(), rows.end()), rows.end());

    Bytes data;
    data.resize(12 + rows.size() * 8);
    std::memcpy(data.data(), kPairsMagic, 8);
    write_u32(data.data() + 8, static_cast<std::uint32_t>(rows.size()));
    if (!rows.empty())
    {
        std::memcpy(data.data() + 12, rows.data(), rows.size() * 8);
    }

    return write_file_atomic(path, data, error);
}

bool write_pairs(const std::filesystem::path& path,
    const std::unordered_map<std::uint32_t, std::uint32_t>& siblings, std::string& error)
{
    std::unordered_map<std::uint32_t, std::vector<std::uint32_t>> converted;
    converted.reserve(siblings.size());
    for (const auto& [bad, good] : siblings)
    {
        converted[bad].push_back(good);
    }
    return write_pairs(path, converted, error);
}

bool read_checkpoint_pairs(const std::filesystem::path& path,
    std::unordered_map<std::uint32_t, std::vector<std::uint32_t>>& pairs, std::string& error)
{
    pairs.clear();

    if (!path_exists(path))
    {
        return true;
    }

    auto bytes = read_file(path, error);
    if (!bytes)
    {
        return false;
    }

    if (bytes->size() < 12 || std::memcmp(bytes->data(), kPairsMagic, 8) != 0)
    {
        error = "bad pair file magic in " + path.string();
        return false;
    }

    const auto count = read_u32(bytes->data() + 8);
    if (bytes->size() < 12 + static_cast<std::size_t>(count) * 8)
    {
        error = "pair file is truncated in " + path.string();
        return false;
    }

    for (std::uint32_t i = 0; i < count; ++i)
    {
        const auto* row = bytes->data() + 12 + static_cast<std::size_t>(i) * 8;
        pairs[read_u32(row)].push_back(read_u32(row + 4));
    }

    return true;
}

bool read_checkpoint_siblings(const std::filesystem::path& path,
    std::unordered_map<std::uint32_t, std::uint32_t>& siblings, std::string& error)
{
    std::unordered_map<std::uint32_t, std::vector<std::uint32_t>> pairs;
    if (!read_checkpoint_pairs(path, pairs, error))
    {
        return false;
    }

    siblings.clear();
    for (const auto& [bad, goods] : pairs)
    {
        if (!goods.empty())
        {
            siblings[bad] = goods.front();
        }
    }
    return true;
}

constexpr int kBuildCheckpointVersion = 1;

struct BuildCheckpoint
{
    std::uint32_t store_format = kStoreFormatVersion;
    std::string source_fingerprint;
    std::uint64_t source_count = 0;
    std::uint64_t partitions = 0;
    std::uint64_t completed_partitions = 0;
    std::uint64_t max_hash_id = 0;
    std::uint64_t tag_count = 0;
    std::uint32_t tag_base = 0;
    std::uint32_t tag_last = 0;
    std::uint64_t hash_count = 0;
    std::uint64_t mapping_count = 0;
    std::int64_t started_at = 0;
};

bool load_build_checkpoint(const std::filesystem::path& path, BuildCheckpoint& checkpoint,
    bool& exists, std::string& error)
{
    exists = false;
    checkpoint = BuildCheckpoint {};

    if (!path_exists(path))
    {
        return true;
    }

    auto bytes = read_file(path, error);
    if (!bytes)
    {
        return false;
    }

    try
    {
        const auto root = nlohmann::json::parse(*bytes);
        if (root.value("version", 0) != kBuildCheckpointVersion)
        {
            error = "unsupported initial-build checkpoint version";
            return false;
        }

        checkpoint.source_fingerprint = root.value("source_fingerprint", std::string());
        checkpoint.store_format = root.value("store_format", static_cast<std::uint32_t>(0));
        checkpoint.source_count = root.value("source_count", static_cast<std::uint64_t>(0));
        checkpoint.partitions = root.value("partitions", static_cast<std::uint64_t>(0));
        checkpoint.completed_partitions = root.value("completed_partitions", static_cast<std::uint64_t>(0));
        checkpoint.max_hash_id = root.value("max_hash_id", static_cast<std::uint64_t>(0));
        checkpoint.tag_count = root.value("tag_count", static_cast<std::uint64_t>(0));
        checkpoint.tag_base = root.value("tag_base", static_cast<std::uint32_t>(0));
        checkpoint.tag_last = root.value("tag_last", static_cast<std::uint32_t>(0));
        checkpoint.hash_count = root.value("hash_count", static_cast<std::uint64_t>(0));
        checkpoint.mapping_count = root.value("mapping_count", static_cast<std::uint64_t>(0));
        checkpoint.started_at = root.value("started_at", static_cast<std::int64_t>(0));
        exists = true;
        return true;
    }
    catch (const std::exception& exception)
    {
        error = std::string("cannot parse initial-build checkpoint: ") + exception.what();
        return false;
    }
}

bool save_build_checkpoint(const std::filesystem::path& path, const BuildCheckpoint& checkpoint,
    std::string& error)
{
    nlohmann::json root;
    root["version"] = kBuildCheckpointVersion;
    root["store_format"] = checkpoint.store_format;
    root["source_fingerprint"] = checkpoint.source_fingerprint;
    root["source_count"] = checkpoint.source_count;
    root["partitions"] = checkpoint.partitions;
    root["completed_partitions"] = checkpoint.completed_partitions;
    root["max_hash_id"] = checkpoint.max_hash_id;
    root["tag_count"] = checkpoint.tag_count;
    root["tag_base"] = checkpoint.tag_base;
    root["tag_last"] = checkpoint.tag_last;
    root["hash_count"] = checkpoint.hash_count;
    root["mapping_count"] = checkpoint.mapping_count;
    root["started_at"] = checkpoint.started_at;

    return write_file_atomic(path, root.dump(), error);
}

std::string source_fingerprint(const std::vector<UpdateSource>& sources)
{
    std::vector<Hash> hashes;
    hashes.reserve(sources.size());
    for (const auto& source : sources)
    {
        hashes.push_back(source.update.hash);
    }
    std::sort(hashes.begin(), hashes.end());

    Bytes bytes;
    bytes.reserve(hashes.size() * kHashSize);
    for (const auto& hash : hashes)
    {
        bytes.insert(bytes.end(), hash.begin(), hash.end());
    }

    return hex_encode(sha256(bytes));
}

// Returns true when store/build.json describes exactly the update snapshot
// persisted in state/repository.json. This is checked before fetching newer
// metadata so a publication that happens during a long build (or while the
// daemon is stopped) cannot invalidate an otherwise resumable checkpoint.
bool checkpoint_matches_metadata(const Config& config, const SyncState& state,
    const RepositoryMetadata& metadata, std::int64_t max_index, BuildCheckpoint& checkpoint)
{
    if (state.built || path_exists(config.store_dir() / "manifest.json"))
    {
        return false;
    }

    bool exists = false;
    std::string checkpoint_error;
    if (!load_build_checkpoint(config.store_dir() / "build.json", checkpoint, exists, checkpoint_error))
    {
        HYD_LOGF(warn, "discarding unreadable initial-build checkpoint: %s",
            checkpoint_error.c_str());
        return false;
    }
    if (!exists)
    {
        return false;
    }

    if (checkpoint.store_format != kStoreFormatVersion
        || checkpoint.partitions != static_cast<std::uint64_t>(config.partitions())
        || checkpoint.completed_partitions > checkpoint.partitions
        || checkpoint.started_at <= 0
        || checkpoint.source_count == 0)
    {
        return false;
    }

    const auto cache = cache_paths(config);
    std::vector<UpdateSource> snapshot;
    snapshot.reserve(metadata.update_file_count());
    for (const auto& [index, rows] : metadata.updates)
    {
        if (max_index >= 0 && index > max_index)
        {
            continue;
        }
        for (const auto& update : rows)
        {
            UpdateSource source;
            source.update = update;
            source.cache_path = update_cache_path(cache, update.hash);
            snapshot.push_back(std::move(source));
        }
    }

    return snapshot.size() == checkpoint.source_count
        && source_fingerprint(snapshot) == checkpoint.source_fingerprint;
}

struct BuildStats
{
    std::uint64_t hash_count = 0;
    std::uint64_t mapping_count = 0;
    std::uint64_t tag_count = 0;
    std::uint32_t tag_base = 0;
    std::uint32_t tag_last = 0;
};

class DefinitionVisitor : public UpdateVisitor
{
public:
    DefinitionVisitor(TagSegmentWriter& tags, std::vector<std::ofstream>& hash_writers, std::size_t partitions,
        std::uint64_t& max_hash_id, std::vector<LegacyHashEntry>& legacy_hashes)
        : tags_(tags), hash_writers_(hash_writers), partitions_(partitions), max_hash_id_(max_hash_id),
          legacy_hashes_(legacy_hashes)
    {
    }

    void on_hash_definition(std::uint32_t hash_id, const Hash& hash) override
    {
        const std::size_t partition = static_cast<std::size_t>(hash[0]) * partitions_ / 256;
        HashEntry entry;
        entry.hash = hash;
        entry.hash_id = hash_id;
        hash_writers_[partition].write(reinterpret_cast<const char*>(&entry), sizeof(entry));
        max_hash_id_ = std::max<std::uint64_t>(max_hash_id_, hash_id);
    }

    void on_unsupported_hash_definition(std::uint32_t hash_id, std::string_view encoded_hash) override
    {
        const auto decoded = hex_decode(encoded_hash);
        if (!decoded || (decoded->size() != 16 && decoded->size() != 20))
        {
            ++invalid_hashes_;
            return;
        }

        legacy_hashes_.push_back(make_legacy_hash_entry(hash_id, *decoded));
        max_hash_id_ = std::max<std::uint64_t>(max_hash_id_, hash_id);
        ++legacy_count_;
    }

    std::uint64_t legacy_count() const { return legacy_count_; }
    std::uint64_t invalid_hashes() const { return invalid_hashes_; }

    void on_invalid_hash_definition(std::uint32_t hash_id, std::string_view encoded_hash) override
    {
        ++invalid_hashes_;
        if (invalid_hashes_ <= 10)
        {
            HYD_LOGF(info, "skipping known-invalid %zu-byte PTR hash definition for hash id %llu (expected): %s",
                encoded_hash.size() / 2, static_cast<unsigned long long>(hash_id),
                std::string(encoded_hash).c_str());
        }
    }

    void on_tag_definition(std::uint32_t tag_id, std::string_view tag) override
    {
        if (!tags_.add(tag_id, tag) && !tags_.error.empty())
        {
            throw std::runtime_error(tags_.error);
        }
    }

private:
    TagSegmentWriter& tags_;
    std::vector<std::ofstream>& hash_writers_;
    std::size_t partitions_;
    std::uint64_t& max_hash_id_;
    std::vector<LegacyHashEntry>& legacy_hashes_;
    std::uint64_t legacy_count_ = 0;
    std::uint64_t invalid_hashes_ = 0;
};

class PartitionVisitor : public UpdateVisitor
{
public:
    PartitionVisitor(std::size_t partition, std::size_t partitions,
        const std::vector<std::uint64_t>& membership, std::unordered_map<std::uint32_t, std::vector<std::uint32_t>>& mappings,
        std::unordered_set<std::uint32_t>& deleted_hashes,
        bool collect_global, std::unordered_map<std::uint32_t, std::uint32_t>& siblings,
        std::unordered_map<std::uint32_t, std::vector<std::uint32_t>>& parents)
        : partition_(partition), partitions_(partitions), membership_(membership), mappings_(mappings),
          deleted_hashes_(deleted_hashes), collect_global_(collect_global), siblings_(siblings), parents_(parents)
    {
    }

    bool in_partition(std::uint32_t hash_id) const
    {
        if ((hash_id >> 6) >= membership_.size())
        {
            return false;
        }
        return (membership_[hash_id >> 6] & (1ull << (hash_id & 63))) != 0;
    }

    void on_mapping_add(std::uint32_t tag_id, std::uint32_t hash_id) override
    {
        if (!in_partition(hash_id))
        {
            return;
        }
        auto& tags = mappings_[hash_id];
        if (std::find(tags.begin(), tags.end(), tag_id) == tags.end())
        {
            tags.push_back(tag_id);
            ++row_count_;
        }
    }

    void on_mapping_delete(std::uint32_t tag_id, std::uint32_t hash_id) override
    {
        if (!in_partition(hash_id))
        {
            return;
        }
        const auto iterator = mappings_.find(hash_id);
        if (iterator == mappings_.end())
        {
            return;
        }
        auto& tags = iterator->second;
        tags.erase(std::remove(tags.begin(), tags.end(), tag_id), tags.end());
        if (tags.empty())
        {
            mappings_.erase(iterator);
        }
    }

    void on_file_delete(std::uint32_t hash_id) override
    {
        if (!in_partition(hash_id))
        {
            return;
        }
        mappings_.erase(hash_id);
        deleted_hashes_.insert(hash_id);
    }

    void on_sibling_add(std::uint32_t bad, std::uint32_t good) override
    {
        if (collect_global_)
        {
            siblings_[bad] = good;
        }
    }

    void on_sibling_delete(std::uint32_t bad, std::uint32_t good) override
    {
        if (!collect_global_)
        {
            return;
        }
        const auto iterator = siblings_.find(bad);
        if (iterator != siblings_.end() && iterator->second == good)
        {
            siblings_.erase(iterator);
        }
    }

    void on_parent_add(std::uint32_t child, std::uint32_t parent) override
    {
        if (!collect_global_)
        {
            return;
        }
        auto& list = parents_[child];
        if (std::find(list.begin(), list.end(), parent) == list.end())
        {
            list.push_back(parent);
        }
    }

    void on_parent_delete(std::uint32_t child, std::uint32_t parent) override
    {
        if (!collect_global_)
        {
            return;
        }
        const auto iterator = parents_.find(child);
        if (iterator == parents_.end())
        {
            return;
        }
        auto& list = iterator->second;
        list.erase(std::remove(list.begin(), list.end(), parent), list.end());
        if (list.empty())
        {
            parents_.erase(iterator);
        }
    }

    std::uint64_t row_count() const { return row_count_; }

private:
    std::size_t partition_;
    std::size_t partitions_;
    const std::vector<std::uint64_t>& membership_;
    std::unordered_map<std::uint32_t, std::vector<std::uint32_t>>& mappings_;
    std::unordered_set<std::uint32_t>& deleted_hashes_;
    bool collect_global_;
    std::unordered_map<std::uint32_t, std::uint32_t>& siblings_;
    std::unordered_map<std::uint32_t, std::vector<std::uint32_t>>& parents_;
    std::uint64_t row_count_ = 0;
};

class DeltaWriter
{
public:
    DeltaWriter() = default;

    ~DeltaWriter()
    {
        if (stream_.is_open())
        {
            stream_.close();
            std::error_code ec;
            std::filesystem::remove(temporary_, ec);
        }
    }

    bool open(const std::filesystem::path& directory, std::uint32_t generation, std::string& error)
    {
        char name[64];
        std::snprintf(name, sizeof(name), "d-%010u.bin", generation);
        final_path_ = directory / name;
        temporary_ = directory / (std::string(name) + ".tmp");

        if (!mkdir_p(directory, error))
        {
            return false;
        }

        stream_.open(temporary_, std::ios::binary | std::ios::trunc);
        if (!stream_)
        {
            error = "cannot create delta file " + temporary_.string();
            return false;
        }
        return true;
    }

    void append(std::uint8_t type, std::span<const std::uint8_t> payload)
    {
        const std::uint32_t length = static_cast<std::uint32_t>(payload.size());
        const std::uint32_t crc = static_cast<std::uint32_t>(crc32(0, payload.data(), length));
        stream_.write(reinterpret_cast<const char*>(&type), 1);
        stream_.write(reinterpret_cast<const char*>(&length), 4);
        if (length > 0)
        {
            stream_.write(reinterpret_cast<const char*>(payload.data()), length);
        }
        stream_.write(reinterpret_cast<const char*>(&crc), 4);
        ++records_;
    }

    bool finish(std::string& error)
    {
        stream_.flush();
        if (!stream_)
        {
            error = "cannot flush delta file";
            return false;
        }
        stream_.close();

        if (!fsync_path(temporary_, error))
        {
            return false;
        }

        if (::rename(temporary_.c_str(), final_path_.c_str()) != 0)
        {
            error = "cannot rename delta file: " + errno_string(errno);
            return false;
        }

        if (!fsync_directory(final_path_.parent_path(), error))
        {
            return false;
        }

        return true;
    }

    std::uint64_t records() const { return records_; }

private:
    std::ofstream stream_;
    std::filesystem::path temporary_;
    std::filesystem::path final_path_;
    std::uint64_t records_ = 0;
};

class DeltaVisitor : public UpdateVisitor
{
public:
    explicit DeltaVisitor(DeltaWriter& writer)
        : writer_(writer)
    {
    }

    void on_hash_definition(std::uint32_t hash_id, const Hash& hash) override
    {
        std::uint8_t payload[36];
        write_u32(payload, hash_id);
        std::memcpy(payload + 4, hash.data(), 32);
        writer_.append(kDeltaHashDefinition, std::span<const std::uint8_t>(payload, sizeof(payload)));
        ++rows_;
    }

    void on_unsupported_hash_definition(std::uint32_t hash_id, std::string_view encoded_hash) override
    {
        const auto decoded = hex_decode(encoded_hash);
        if (!decoded || (decoded->size() != 16 && decoded->size() != 20))
        {
            return;
        }

        std::uint8_t payload[37] = {};
        write_u32(payload, hash_id);
        payload[4] = static_cast<std::uint8_t>(decoded->size());
        std::memcpy(payload + 5, decoded->data(), decoded->size());
        writer_.append(kDeltaLegacyHashDefinition, std::span<const std::uint8_t>(payload, sizeof(payload)));
        ++rows_;
    }

    void on_invalid_hash_definition(std::uint32_t hash_id, std::string_view encoded_hash) override
    {
        if (invalid_hashes_ < 10)
        {
            HYD_LOGF(info, "skipping known-invalid %zu-byte PTR hash definition for hash id %llu (expected): %s",
                encoded_hash.size() / 2, static_cast<unsigned long long>(hash_id),
                std::string(encoded_hash).c_str());
        }
        ++invalid_hashes_;
    }

    void on_tag_definition(std::uint32_t tag_id, std::string_view tag) override
    {
        if (tag.size() > 0xffff)
        {
            throw std::runtime_error("tag is too long");
        }
        Bytes payload;
        payload.resize(6 + tag.size());
        write_u32(payload.data(), tag_id);
        payload[4] = static_cast<std::uint8_t>(tag.size() & 0xff);
        payload[5] = static_cast<std::uint8_t>((tag.size() >> 8) & 0xff);
        std::memcpy(payload.data() + 6, tag.data(), tag.size());
        writer_.append(kDeltaTagDefinition, payload);
        ++rows_;
    }

    void on_mapping_add(std::uint32_t tag_id, std::uint32_t hash_id) override
    {
        std::uint8_t payload[8];
        write_u32(payload, hash_id);
        write_u32(payload + 4, tag_id);
        writer_.append(kDeltaMappingAdd, std::span<const std::uint8_t>(payload, sizeof(payload)));
        ++rows_;
    }

    void on_mapping_delete(std::uint32_t tag_id, std::uint32_t hash_id) override
    {
        std::uint8_t payload[8];
        write_u32(payload, hash_id);
        write_u32(payload + 4, tag_id);
        writer_.append(kDeltaMappingDelete, std::span<const std::uint8_t>(payload, sizeof(payload)));
        ++rows_;
    }

    void on_file_delete(std::uint32_t hash_id) override
    {
        std::uint8_t payload[4];
        write_u32(payload, hash_id);
        writer_.append(kDeltaFileDelete, std::span<const std::uint8_t>(payload, sizeof(payload)));
        ++rows_;
    }

    void on_sibling_add(std::uint32_t bad, std::uint32_t good) override
    {
        append_pair(kDeltaSiblingAdd, bad, good);
    }

    void on_sibling_delete(std::uint32_t bad, std::uint32_t good) override
    {
        append_pair(kDeltaSiblingDelete, bad, good);
    }

    void on_parent_add(std::uint32_t child, std::uint32_t parent) override
    {
        append_pair(kDeltaParentAdd, child, parent);
    }

    void on_parent_delete(std::uint32_t child, std::uint32_t parent) override
    {
        append_pair(kDeltaParentDelete, child, parent);
    }

    std::uint64_t rows() const { return rows_; }

private:
    void append_pair(std::uint8_t type, std::uint32_t first, std::uint32_t second)
    {
        std::uint8_t payload[8];
        write_u32(payload, first);
        write_u32(payload + 4, second);
        writer_.append(type, std::span<const std::uint8_t>(payload, sizeof(payload)));
        ++rows_;
    }

    DeltaWriter& writer_;
    std::uint64_t rows_ = 0;
    std::uint64_t invalid_hashes_ = 0;
};

bool save_update_kinds(const std::filesystem::path& path,
    const std::unordered_map<std::string, int>& kinds, std::string& error)
{
    nlohmann::json root = nlohmann::json::object();
    for (const auto& [hash, kind] : kinds)
    {
        root[hash] = kind;
    }
    return write_file_atomic(path, root.dump(), error);
}

bool load_update_kinds(const std::filesystem::path& path,
    std::unordered_map<std::string, int>& kinds, std::string& error)
{
    kinds.clear();
    if (!path_exists(path))
    {
        return true;
    }
    auto bytes = read_file(path, error);
    if (!bytes)
    {
        return false;
    }
    try
    {
        const auto root = nlohmann::json::parse(*bytes);
        for (auto iterator = root.begin(); iterator != root.end(); ++iterator)
        {
            kinds[iterator.key()] = iterator.value().get<int>();
        }
        return true;
    }
    catch (const std::exception& exception)
    {
        error = std::string("cannot parse update kinds: ") + exception.what();
        return false;
    }
}

bool detect_kind(UpdateParser& parser, std::span<const std::uint8_t> compressed,
    std::unordered_map<std::string, int>& cache, const Hash& hash, UpdateKind& kind, std::string& error)
{
    const std::string key = hex_encode(hash);
    const auto iterator = cache.find(key);
    if (iterator != cache.end())
    {
        kind = static_cast<UpdateKind>(iterator->second);
        return true;
    }

    class NoopVisitor : public UpdateVisitor {};

    NoopVisitor visitor;
    if (!parser.parse(compressed, visitor, kind, error))
    {
        return false;
    }
    cache[key] = static_cast<int>(kind);
    return true;
}

bool build_initial_store(const Config& config, const RepositoryMetadata& metadata,
    const std::vector<UpdateSource>& sources, const SyncCallbacks& callbacks,
    SyncState& state, std::string& error)
{
    const auto store_dir = config.store_dir();
    const auto state_dir = config.state_dir();
    const std::size_t partitions = static_cast<std::size_t>(config.partitions());

    BuildProgress* progress = callbacks.progress.get();
    BuildProgressGuard progress_guard { progress };

    std::vector<std::uint64_t> source_sizes(sources.size());
    std::uint64_t sources_total_bytes = 0;
    for (std::size_t i = 0; i < sources.size(); ++i)
    {
        source_sizes[i] = file_size_or_zero(sources[i].cache_path);
        sources_total_bytes += source_sizes[i];
    }

    const auto checkpoint_path = store_dir / "build.json";
    const auto fingerprint = source_fingerprint(sources);

    BuildCheckpoint checkpoint;
    bool checkpoint_exists = false;
    if (!load_build_checkpoint(checkpoint_path, checkpoint, checkpoint_exists, error))
    {
        HYD_LOGF(warn, "discarding unreadable initial-build checkpoint: %s", error.c_str());
        error.clear();
        checkpoint = BuildCheckpoint {};
        checkpoint_exists = false;
    }

    bool resume = checkpoint_exists
        && checkpoint.store_format == kStoreFormatVersion
        && checkpoint.source_fingerprint == fingerprint
        && checkpoint.source_count == sources.size()
        && checkpoint.partitions == partitions
        && checkpoint.completed_partitions <= partitions
        && checkpoint.started_at > 0
        && !path_exists(store_dir / "manifest.json");

    if (resume)
    {
        // The tag segment, kinds cache, legacy list, and every finished or
        // unfinished partition artefact must still be present.
        resume = path_exists(store_dir / "tags" / "seg-000000.idx")
            && path_exists(store_dir / "tags" / "seg-000000.str")
            && path_exists(store_dir / "tags" / "seg-000000.ids")
            && path_exists(store_dir / "legacy.bin")
            && path_exists(state_dir / "update_kinds.json")
            && (checkpoint.completed_partitions == 0
                || (path_exists(store_dir / "siblings.bin")
                    && path_exists(store_dir / "parents.bin")));

        for (std::size_t p = 0; resume && p < partitions; ++p)
        {
            char name[32];
            std::snprintf(name, sizeof(name), "p%02zu", p);
            if (p < checkpoint.completed_partitions)
            {
                resume = path_exists(store_dir / name / "hashes.bin")
                    && path_exists(store_dir / name / "maps.bin");
            }
            else
            {
                std::snprintf(name, sizeof(name), "defs-%02zu.bin", p);
                resume = path_exists(store_dir / name);
            }
        }
    }

    if (checkpoint_exists && !resume)
    {
        HYD_LOG(info, "discarding a stale initial-build checkpoint; rebuilding from scratch");
    }

    const std::int64_t build_started_at = resume ? checkpoint.started_at : now_unix();

    if (progress != nullptr)
    {
        if (resume)
        {
            progress->phase.store(BuildProgress::phase_partitions);
            progress->partition.store(static_cast<int>(
                std::min<std::size_t>(checkpoint.completed_partitions + 1, partitions)));
            progress->files_done.store(0);
            progress->files_total.store(0);
            progress->bytes_done.store(0);
            progress->bytes_total.store(0);
            progress->hashes.store(0);
            progress->mappings.store(0);
            progress->started_at.store(build_started_at);
            progress->partition_started_at.store(0);
            progress->updated_at.store(now_unix());
            progress->partitions.store(static_cast<int>(partitions));
        }
        else
        {
            progress->phase.store(BuildProgress::phase_definitions);
            progress->partition.store(0);
            progress->partitions.store(static_cast<int>(partitions));
            progress->files_done.store(0);
            progress->files_total.store(sources.size());
            progress->bytes_done.store(0);
            progress->bytes_total.store(sources_total_bytes);
            progress->hashes.store(0);
            progress->mappings.store(0);
            progress->started_at.store(build_started_at);
            progress->partition_started_at.store(0);
            progress->updated_at.store(now_unix());
        }
    }

    std::error_code ec;
    if (!resume)
    {
        std::filesystem::remove_all(store_dir, ec);
        if (ec)
        {
            error = "cannot clear old store: " + ec.message();
            return false;
        }
    }

    if (!mkdir_p(store_dir / "tags", error) || !mkdir_p(store_dir / "delta", error))
    {
        return false;
    }
    for (std::size_t p = 0; p < partitions; ++p)
    {
        char name[32];
        std::snprintf(name, sizeof(name), "p%02zu", p);
        if (!mkdir_p(store_dir / name, error))
        {
            return false;
        }
    }

    if (resume)
    {
        HYD_LOGF(info, "resuming initial build at partition %llu/%zu",
            static_cast<unsigned long long>(checkpoint.completed_partitions + 1), partitions);
    }
    else
    {
        HYD_LOGF(info, "initial sync: building tag dictionary and hash definitions from %zu update files",
            sources.size());
    }

    UpdateParser parser;
    std::unordered_map<std::string, int> kinds;
    std::uint64_t max_hash_id = 0;
    std::vector<LegacyHashEntry> legacy_hashes;
    BuildStats stats;

    if (!resume)
    {
        TagSegmentWriter tag_writer;
        if (!tag_writer.open(store_dir / "tags", 0))
        {
            error = tag_writer.error;
            return false;
        }

        std::vector<std::ofstream> hash_writers(partitions);
        for (std::size_t p = 0; p < partitions; ++p)
        {
            char name[32];
            std::snprintf(name, sizeof(name), "defs-%02zu.bin", p);
            hash_writers[p].open(store_dir / name, std::ios::binary | std::ios::trunc);
            if (!hash_writers[p])
            {
                error = "cannot create temporary hash definition file";
                return false;
            }
        }

        DefinitionVisitor definition_visitor(
            tag_writer, hash_writers, partitions, max_hash_id, legacy_hashes);

        std::size_t processed = 0;
        std::uint64_t processed_bytes = 0;
        auto last_report = std::chrono::steady_clock::now();

        for (std::size_t source_index = 0; source_index < sources.size(); ++source_index)
        {
            const auto& source = sources[source_index];

            if (stopped(callbacks))
            {
                error = "stopped";
                return false;
            }

            auto bytes = read_file(source.cache_path, error);
            if (!bytes)
            {
                return false;
            }

            UpdateKind kind = UpdateKind::unknown;
            if (!parser.parse(*bytes, definition_visitor, kind, error))
            {
                error = "update " + hex_encode(source.update.hash) + ": " + error;
                return false;
            }
            kinds[hex_encode(source.update.hash)] = static_cast<int>(kind);
            ++processed;
            processed_bytes += source_sizes[source_index];

            if (progress != nullptr)
            {
                progress->files_done.store(processed);
                progress->bytes_done.store(processed_bytes);
                progress->updated_at.store(now_unix());
            }

            const auto now = std::chrono::steady_clock::now();
            if (std::chrono::duration<double>(now - last_report).count() > 10.0)
            {
                const auto elapsed = now_unix() - build_started_at;
                const auto eta = processed > 0
                    ? elapsed * static_cast<std::int64_t>(sources.size() - processed)
                        / static_cast<std::int64_t>(processed)
                    : 0;
                HYD_LOGF(info,
                    "definitions: %zu/%zu update files (%.1f%%), %s/%s, elapsed %s, ETA %s",
                    processed, sources.size(),
                    progress_percent(processed, sources.size()),
                    human_bytes(processed_bytes).c_str(),
                    human_bytes(sources_total_bytes).c_str(),
                    format_eta(elapsed).c_str(),
                    format_eta(eta).c_str());
                last_report = now;
            }
        }

        for (auto& writer : hash_writers)
        {
            writer.flush();
            if (!writer)
            {
                error = "cannot flush temporary hash definition file";
                return false;
            }
            writer.close();
        }

        if (!legacy_hashes.empty())
        {
            HYD_LOGF(info, "found %llu legacy sha1/md5 hash definitions",
                static_cast<unsigned long long>(legacy_hashes.size()));
        }
        if (definition_visitor.invalid_hashes() > 0)
        {
            HYD_LOGF(info, "skipped %llu known-invalid PTR hash definitions (expected)",
                static_cast<unsigned long long>(definition_visitor.invalid_hashes()));
        }

        if (!tag_writer.finish(error))
        {
            return false;
        }

        if (!save_update_kinds(state_dir / "update_kinds.json", kinds, error))
        {
            return false;
        }

        stats.tag_count = tag_writer.count;
        stats.tag_base = static_cast<std::uint32_t>(tag_writer.base_id);
        stats.tag_last = tag_writer.last_id;

        if (!write_legacy_hashes(store_dir / "legacy.bin", legacy_hashes, error))
        {
            return false;
        }

        BuildCheckpoint next;
        next.source_fingerprint = fingerprint;
        next.source_count = sources.size();
        next.partitions = partitions;
        next.completed_partitions = 0;
        next.max_hash_id = max_hash_id;
        next.tag_count = stats.tag_count;
        next.tag_base = stats.tag_base;
        next.tag_last = stats.tag_last;
        next.hash_count = 0;
        next.mapping_count = 0;
        next.started_at = build_started_at;
        if (!save_build_checkpoint(checkpoint_path, next, error))
        {
            return false;
        }
        checkpoint = std::move(next);
    }
    else
    {
        if (!load_update_kinds(state_dir / "update_kinds.json", kinds, error))
        {
            return false;
        }
        if (kinds.empty() && !sources.empty())
        {
            error = "initial-build checkpoint has no update kinds";
            return false;
        }
        if (!read_legacy_hashes(store_dir / "legacy.bin", legacy_hashes, error))
        {
            return false;
        }
        max_hash_id = checkpoint.max_hash_id;
        stats.tag_count = checkpoint.tag_count;
        stats.tag_base = checkpoint.tag_base;
        stats.tag_last = checkpoint.tag_last;
        stats.hash_count = checkpoint.hash_count;
        stats.mapping_count = checkpoint.mapping_count;
    }

    std::uint64_t content_total_files = 0;
    std::uint64_t content_total_bytes = 0;
    for (std::size_t i = 0; i < sources.size(); ++i)
    {
        const auto kind_iterator = kinds.find(hex_encode(sources[i].update.hash));
        if (kind_iterator != kinds.end()
            && kind_iterator->second != static_cast<int>(UpdateKind::content))
        {
            continue;
        }
        ++content_total_files;
        content_total_bytes += source_sizes[i];
    }

    if (progress != nullptr)
    {
        progress->phase.store(BuildProgress::phase_partitions);
        progress->partition.store(static_cast<int>(checkpoint.completed_partitions));
        progress->files_done.store(0);
        progress->files_total.store(content_total_files);
        progress->bytes_done.store(0);
        progress->bytes_total.store(content_total_bytes);
        progress->hashes.store(0);
        progress->mappings.store(0);
        progress->partition_started_at.store(0);
        progress->updated_at.store(now_unix());
    }

    HYD_LOGF(info, "initial sync: %llu content update files (%s) across %zu partitions",
        static_cast<unsigned long long>(content_total_files),
        human_bytes(content_total_bytes).c_str(), partitions);

    std::unordered_map<std::uint32_t, std::uint32_t> siblings;
    std::unordered_map<std::uint32_t, std::vector<std::uint32_t>> parents;

    if (resume && checkpoint.completed_partitions > 0)
    {
        if (!read_checkpoint_siblings(store_dir / "siblings.bin", siblings, error))
        {
            return false;
        }
        if (!read_checkpoint_pairs(store_dir / "parents.bin", parents, error))
        {
            return false;
        }
    }

    // Phase B: one pass per partition over all content updates.
    const std::size_t starting_partition = resume
        ? static_cast<std::size_t>(checkpoint.completed_partitions)
        : 0;

    for (std::size_t p = starting_partition; p < partitions; ++p)
    {
        if (stopped(callbacks))
        {
            error = "stopped";
            return false;
        }

        HYD_LOGF(info, "partition %zu/%zu: reading hash definitions", p + 1, partitions);

        const std::int64_t partition_started_at = now_unix();
        if (progress != nullptr)
        {
            progress->partition.store(static_cast<int>(p + 1));
            progress->files_done.store(0);
            progress->bytes_done.store(0);
            progress->hashes.store(0);
            progress->mappings.store(0);
            progress->partition_started_at.store(partition_started_at);
            progress->updated_at.store(partition_started_at);
        }

        char defs_name[32];
        std::snprintf(defs_name, sizeof(defs_name), "defs-%02zu.bin", p);
        const auto defs_path = store_dir / defs_name;

        std::vector<HashEntry> hash_entries;
        {
            auto defs_bytes = read_file(defs_path, error);
            if (!defs_bytes)
            {
                return false;
            }
            if (defs_bytes->size() % sizeof(HashEntry) != 0)
            {
                error = "temporary hash definition file is corrupt";
                return false;
            }
            hash_entries.resize(defs_bytes->size() / sizeof(HashEntry));
            if (!hash_entries.empty())
            {
                std::memcpy(hash_entries.data(), defs_bytes->data(), defs_bytes->size());
            }
        }

        std::vector<std::uint64_t> membership;
        membership.assign(max_hash_id / 64 + 2, 0);
        for (const auto& entry : hash_entries)
        {
            membership[entry.hash_id >> 6] |= (1ull << (entry.hash_id & 63));
        }
        for (const auto& entry : legacy_hashes)
        {
            const std::size_t entry_partition =
                static_cast<std::size_t>(entry.hash[0]) * partitions / 256;
            if (entry_partition == p)
            {
                membership[entry.hash_id >> 6] |= (1ull << (entry.hash_id & 63));
            }
        }

        std::unordered_map<std::uint32_t, std::vector<std::uint32_t>> mappings;
        mappings.reserve(hash_entries.size());
        std::unordered_set<std::uint32_t> deleted_hashes;

        if (progress != nullptr)
        {
            progress->hashes.store(hash_entries.size());
        }

        HYD_LOGF(info, "partition %zu/%zu: %llu content update files (%s)",
            p + 1, partitions, static_cast<unsigned long long>(content_total_files),
            human_bytes(content_total_bytes).c_str());

        PartitionVisitor partition_visitor(p, partitions, membership, mappings, deleted_hashes,
            p == 0, siblings, parents);

        std::size_t content_files = 0;
        std::uint64_t content_bytes = 0;
        auto last_partition_report = std::chrono::steady_clock::now();
        for (std::size_t source_index = 0; source_index < sources.size(); ++source_index)
        {
            const auto& source = sources[source_index];

            if (stopped(callbacks))
            {
                error = "stopped";
                return false;
            }

            const auto kind_iterator = kinds.find(hex_encode(source.update.hash));
            if (kind_iterator != kinds.end()
                && kind_iterator->second != static_cast<int>(UpdateKind::content))
            {
                continue;
            }

            auto bytes = read_file(source.cache_path, error);
            if (!bytes)
            {
                return false;
            }

            UpdateKind kind = UpdateKind::unknown;
            if (!parser.parse(*bytes, partition_visitor, kind, error))
            {
                error = "update " + hex_encode(source.update.hash) + ": " + error;
                return false;
            }
            if (kind == UpdateKind::content)
            {
                ++content_files;
                content_bytes += source_sizes[source_index];

                if (progress != nullptr)
                {
                    progress->files_done.store(content_files);
                    progress->bytes_done.store(content_bytes);
                    progress->mappings.store(partition_visitor.row_count());
                    progress->updated_at.store(now_unix());
                }
            }

            const auto now = std::chrono::steady_clock::now();
            if (std::chrono::duration<double>(now - last_partition_report).count() > 10.0)
            {
                const auto elapsed = now_unix() - partition_started_at;
                const auto eta = content_files > 0
                    ? elapsed * static_cast<std::int64_t>(content_total_files - content_files)
                        / static_cast<std::int64_t>(content_files)
                    : 0;
                HYD_LOGF(info,
                    "partition %zu/%zu: %zu/%llu content files (%.1f%%), %s/%s, "
                    "%llu mappings, elapsed %s, ETA %s",
                    p + 1, partitions, content_files,
                    static_cast<unsigned long long>(content_total_files),
                    progress_percent(content_files, content_total_files),
                    human_bytes(content_bytes).c_str(),
                    human_bytes(content_total_bytes).c_str(),
                    static_cast<unsigned long long>(partition_visitor.row_count()),
                    format_eta(elapsed).c_str(), format_eta(eta).c_str());
                last_partition_report = now;
            }
        }

        for (auto hash_id : deleted_hashes)
        {
            hash_entries.erase(std::remove_if(hash_entries.begin(), hash_entries.end(),
                                   [hash_id](const HashEntry& entry) { return entry.hash_id == hash_id; }),
                hash_entries.end());
        }

        if (!deleted_hashes.empty())
        {
            legacy_hashes.erase(std::remove_if(legacy_hashes.begin(), legacy_hashes.end(),
                                     [&deleted_hashes](const LegacyHashEntry& entry) {
                                         return deleted_hashes.count(entry.hash_id) != 0;
                                     }),
                legacy_hashes.end());
        }

        if (progress != nullptr)
        {
            progress->hashes.store(hash_entries.size());
            progress->mappings.store(partition_visitor.row_count());
            progress->updated_at.store(now_unix());
        }

        HYD_LOGF(info,
            "partition %zu/%zu complete: %zu hashes, %zu mappings "
            "(%zu content files, %s) in %s",
            p + 1, partitions, hash_entries.size(), mappings.size(), content_files,
            human_bytes(content_bytes).c_str(),
            format_eta(now_unix() - partition_started_at).c_str());

        char partition_name[32];
        std::snprintf(partition_name, sizeof(partition_name), "p%02zu", p);
        const auto partition_dir = store_dir / partition_name;

        if (!write_partition_hashes(partition_dir / "hashes.bin", hash_entries, error))
        {
            return false;
        }
        if (!write_partition_maps(partition_dir / "maps.bin", mappings, error))
        {
            return false;
        }

        stats.hash_count += hash_entries.size();
        for (const auto& [hash_id, tags] : mappings)
        {
            (void)hash_id;
            stats.mapping_count += tags.size();
        }

        if (p == 0)
        {
            if (!write_pairs(store_dir / "siblings.bin", siblings, error))
            {
                return false;
            }
            if (!write_pairs(store_dir / "parents.bin", parents, error))
            {
                return false;
            }
        }

        if (!write_legacy_hashes(store_dir / "legacy.bin", legacy_hashes, error))
        {
            return false;
        }

        BuildCheckpoint next;
        next.source_fingerprint = fingerprint;
        next.source_count = sources.size();
        next.partitions = partitions;
        next.completed_partitions = p + 1;
        next.max_hash_id = max_hash_id;
        next.tag_count = stats.tag_count;
        next.tag_base = stats.tag_base;
        next.tag_last = stats.tag_last;
        next.hash_count = stats.hash_count;
        next.mapping_count = stats.mapping_count;
        next.started_at = build_started_at;
        if (!save_build_checkpoint(checkpoint_path, next, error))
        {
            return false;
        }
        checkpoint = std::move(next);

        HYD_LOGF(info, "checkpoint: partition %zu/%zu complete", p + 1, partitions);

        // Test hook: gives integration tests a deterministic window in which
        // to kill the daemon between two partitions. No effect when unset.
        if (const char* pause = std::getenv("HYD_TEST_PAUSE_AFTER_CHECKPOINT_MS"))
        {
            const long milliseconds = std::strtol(pause, nullptr, 10);
            if (milliseconds > 0)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
            }
        }

        std::error_code remove_ec;
        std::filesystem::remove(defs_path, remove_ec);

        hash_entries.clear();
        hash_entries.shrink_to_fit();
        mappings.clear();
        membership.clear();
        membership.shrink_to_fit();
    }

    if (!write_legacy_hashes(store_dir / "legacy.bin", legacy_hashes, error))
    {
        return false;
    }
    stats.hash_count += legacy_hashes.size();

    if (!legacy_hashes.empty())
    {
        HYD_LOGF(info, "indexed %llu legacy sha1/md5 hash definitions",
            static_cast<unsigned long long>(legacy_hashes.size()));
    }

    if (!write_pairs(store_dir / "siblings.bin", siblings, error))
    {
        return false;
    }
    if (!write_pairs(store_dir / "parents.bin", parents, error))
    {
        return false;
    }

    for (std::size_t p = 0; p < partitions; ++p)
    {
        char name[32];
        std::snprintf(name, sizeof(name), "defs-%02zu.bin", p);
        std::error_code remove_ec;
        std::filesystem::remove(store_dir / name, remove_ec);
    }

    StoreManifest manifest;
    manifest.format = kStoreFormatVersion;
    manifest.partitions = static_cast<std::uint32_t>(partitions);
    manifest.hash_count = stats.hash_count;
    manifest.mapping_count = stats.mapping_count;
    manifest.tag_count = stats.tag_count;
    manifest.built_at = now_unix();
    manifest.tag_segments.push_back(StoreManifest::TagSegmentInfo {
        stats.tag_base,
        stats.tag_count,
        stats.tag_last,
        "tags/seg-000000.idx",
        "tags/seg-000000.str",
        "tags/seg-000000.ids",
    });

    if (!save_store_manifest(store_dir, manifest, error))
    {
        return false;
    }

    state.built = true;
    state.built_at = manifest.built_at;
    state.hash_count = stats.hash_count;
    state.mapping_count = stats.mapping_count;
    state.tag_count = stats.tag_count;

    return true;
}

bool download_updates(const Config& config, const std::vector<UpdateSource>& sources,
    const SyncCallbacks& callbacks, SyncState& state, RepositoryClient& repository, std::string& error)
{
    (void)repository;

    std::vector<DownloadItem> items;
    items.reserve(sources.size());
    for (const auto& source : sources)
    {
        items.push_back(DownloadItem { source.update.hash, source.cache_path });
    }

    const auto summary = download_update_files(config, items, callbacks.should_stop);
    state.downloaded_bytes += summary.bytes;
    state.downloaded_files += summary.files;

    if (!summary.error.empty())
    {
        error = summary.error;
        return false;
    }
    if (summary.stopped)
    {
        error = "stopped";
        return false;
    }
    return true;
}

bool apply_incremental(const Config& config, const std::vector<UpdateSource>& sources,
    const SyncCallbacks& callbacks, SyncState& state, std::string& error)
{
    const auto store_dir = config.store_dir();
    const auto state_dir = config.state_dir();
    const auto partitions = static_cast<std::size_t>(config.partitions());

    UpdateParser parser;
    std::unordered_map<std::string, int> kinds;
    if (!load_update_kinds(state_dir / "update_kinds.json", kinds, error))
    {
        return false;
    }

    DeltaWriter writer;
    if (!writer.open(store_dir / "delta", state.delta_generation + 1, error))
    {
        return false;
    }

    DeltaVisitor delta_visitor(writer);

    for (const auto& source : sources)
    {
        if (stopped(callbacks))
        {
            error = "stopped";
            return false;
        }

        auto bytes = read_file(source.cache_path, error);
        if (!bytes)
        {
            return false;
        }

        UpdateKind kind = UpdateKind::unknown;
        if (!detect_kind(parser, *bytes, kinds, source.update.hash, kind, error))
        {
            error = "update " + hex_encode(source.update.hash) + ": " + error;
            return false;
        }

        if (kind != UpdateKind::content)
        {
            continue;
        }
    }

    // Definitions must be applied before content within an index. We sort the
    // work by index and do two passes per index.
    std::map<std::int64_t, std::vector<const UpdateSource*>> by_index;
    for (const auto& source : sources)
    {
        by_index[source.update.index].push_back(&source);
    }

    for (const auto& [index, group] : by_index)
    {
        (void)index;
        if (stopped(callbacks))
        {
            error = "stopped";
            return false;
        }

        // Definitions first.
        for (const auto* source : group)
        {
            const std::string key = hex_encode(source->update.hash);
            const auto kind_iterator = kinds.find(key);
            if (kind_iterator == kinds.end() || kind_iterator->second != static_cast<int>(UpdateKind::definitions))
            {
                continue;
            }

            auto bytes = read_file(source->cache_path, error);
            if (!bytes)
            {
                return false;
            }
            UpdateKind kind = UpdateKind::unknown;
            if (!parser.parse(*bytes, delta_visitor, kind, error))
            {
                error = "update " + key + ": " + error;
                return false;
            }
        }

        // Then content.
        for (const auto* source : group)
        {
            const std::string key = hex_encode(source->update.hash);
            const auto kind_iterator = kinds.find(key);
            if (kind_iterator == kinds.end() || kind_iterator->second != static_cast<int>(UpdateKind::content))
            {
                continue;
            }

            auto bytes = read_file(source->cache_path, error);
            if (!bytes)
            {
                return false;
            }
            UpdateKind kind = UpdateKind::unknown;
            if (!parser.parse(*bytes, delta_visitor, kind, error))
            {
                error = "update " + key + ": " + error;
                return false;
            }
        }
    }

    if (!writer.finish(error))
    {
        return false;
    }

    if (!save_update_kinds(state_dir / "update_kinds.json", kinds, error))
    {
        return false;
    }

    HYD_LOGF(info, "applied %llu rows into delta generation %u",
        static_cast<unsigned long long>(delta_visitor.rows()), state.delta_generation + 1);

    ++state.delta_generation;
    (void)partitions;
    return true;
}

} // namespace


bool read_base_partition(const std::filesystem::path& directory, std::vector<HashEntry>& hashes,
    std::unordered_map<std::uint32_t, std::vector<std::uint32_t>>& mappings, std::string& error)
{
    auto hash_bytes = read_file(directory / "hashes.bin", error);
    if (!hash_bytes)
    {
        return false;
    }
    if (hash_bytes->size() < 12 || std::memcmp(hash_bytes->data(), kHashesMagic, 8) != 0)
    {
        error = "bad hashes.bin in " + directory.string();
        return false;
    }
    const auto hash_count = read_u32(hash_bytes->data() + 8);
    if (hash_bytes->size() < 12 + static_cast<std::size_t>(hash_count) * sizeof(HashEntry))
    {
        error = "truncated hashes.bin in " + directory.string();
        return false;
    }
    hashes.resize(hash_count);
    if (hash_count > 0)
    {
        std::memcpy(hashes.data(), hash_bytes->data() + 12, hash_count * sizeof(HashEntry));
    }

    auto map_bytes = read_file(directory / "maps.bin", error);
    if (!map_bytes)
    {
        return false;
    }
    if (map_bytes->size() < 20 || std::memcmp(map_bytes->data(), kMapsMagic, 8) != 0)
    {
        error = "bad maps.bin in " + directory.string();
        return false;
    }
    const auto map_count = read_u32(map_bytes->data() + 8);
    const auto tag_count = read_u32(map_bytes->data() + 12);
    const std::size_t expected = 20 + static_cast<std::size_t>(map_count) * 4
        + (static_cast<std::size_t>(map_count) + 1) * 8 + static_cast<std::size_t>(tag_count) * 4;
    if (map_bytes->size() < expected)
    {
        error = "truncated maps.bin in " + directory.string();
        return false;
    }

    const auto* ids = map_bytes->data() + 20;
    const auto* offsets = ids + static_cast<std::size_t>(map_count) * 4;
    const auto* tags = offsets + (static_cast<std::size_t>(map_count) + 1) * 8;

    mappings.clear();
    mappings.reserve(map_count);
    for (std::uint32_t i = 0; i < map_count; ++i)
    {
        const auto hash_id = read_u32(ids + i * 4);
        const auto begin = read_u64(offsets + i * 8);
        const auto end = read_u64(offsets + (i + 1) * 8);
        if (begin > end || end > tag_count)
        {
            error = "corrupt maps.bin offsets in " + directory.string();
            return false;
        }
        auto& list = mappings[hash_id];
        list.reserve(static_cast<std::size_t>(end - begin));
        for (std::uint64_t j = begin; j < end; ++j)
        {
            list.push_back(read_u32(tags + j * 4));
        }
    }

    return true;
}

bool read_pairs(const std::filesystem::path& path,
    std::unordered_map<std::uint32_t, std::vector<std::uint32_t>>& pairs, std::string& error)
{
    pairs.clear();
    if (!path_exists(path))
    {
        return true;
    }
    auto bytes = read_file(path, error);
    if (!bytes)
    {
        return false;
    }
    if (bytes->size() < 12 || std::memcmp(bytes->data(), kPairsMagic, 8) != 0)
    {
        error = "bad tag pair file " + path.string();
        return false;
    }
    const auto count = read_u32(bytes->data() + 8);
    if (bytes->size() < 12 + static_cast<std::size_t>(count) * 8)
    {
        error = "truncated tag pair file " + path.string();
        return false;
    }
    for (std::uint32_t i = 0; i < count; ++i)
    {
        const auto* row = bytes->data() + 12 + i * 8;
        const auto first = read_u32(row);
        const auto second = read_u32(row + 4);
        auto& list = pairs[first];
        if (std::find(list.begin(), list.end(), second) == list.end())
        {
            list.push_back(second);
        }
    }
    return true;
}

void remove_mapping(std::vector<std::uint32_t>& tags, std::uint32_t tag_id)
{
    tags.erase(std::remove(tags.begin(), tags.end(), tag_id), tags.end());
}

void add_mapping(std::vector<std::uint32_t>& tags, std::uint32_t tag_id)
{
    if (std::find(tags.begin(), tags.end(), tag_id) == tags.end())
    {
        tags.push_back(tag_id);
    }
}

bool load_sync_state(const std::filesystem::path& path, SyncState& state, std::string& error)
{
    if (!path_exists(path))
    {
        return true;
    }

    auto bytes = read_file(path, error);
    if (!bytes)
    {
        return false;
    }

    try
    {
        const auto root = nlohmann::json::parse(*bytes);
        state.next_index = root.value("next_index", static_cast<std::int64_t>(0));
        state.built = root.value("built", false);
        state.partial = root.value("partial", false);
        state.built_at = root.value("built_at", static_cast<std::int64_t>(0));
        state.last_sync_at = root.value("last_sync_at", static_cast<std::int64_t>(0));
        state.last_sync_started = root.value("last_sync_started", static_cast<std::int64_t>(0));
        state.next_update_due = root.value("next_update_due", static_cast<std::int64_t>(0));
        state.downloaded_bytes = root.value("downloaded_bytes", static_cast<std::uint64_t>(0));
        state.downloaded_files = root.value("downloaded_files", static_cast<std::uint64_t>(0));
        state.delta_generation = root.value("delta_generation", 0u);
        state.hash_count = root.value("hash_count", static_cast<std::uint64_t>(0));
        state.mapping_count = root.value("mapping_count", static_cast<std::uint64_t>(0));
        state.tag_count = root.value("tag_count", static_cast<std::uint64_t>(0));
        state.last_compacted_at = root.value("last_compacted_at", static_cast<std::int64_t>(0));
        return true;
    }
    catch (const std::exception& exception)
    {
        error = std::string("cannot parse sync state: ") + exception.what();
        return false;
    }
}

bool save_sync_state(const std::filesystem::path& path, const SyncState& state, std::string& error)
{
    nlohmann::json root;
    root["next_index"] = state.next_index;
    root["built"] = state.built;
    root["partial"] = state.partial;
    root["built_at"] = state.built_at;
    root["last_sync_at"] = state.last_sync_at;
    root["last_sync_started"] = state.last_sync_started;
    root["next_update_due"] = state.next_update_due;
    root["downloaded_bytes"] = state.downloaded_bytes;
    root["downloaded_files"] = state.downloaded_files;
    root["delta_generation"] = state.delta_generation;
    root["hash_count"] = state.hash_count;
    root["mapping_count"] = state.mapping_count;
    root["tag_count"] = state.tag_count;
    root["last_compacted_at"] = state.last_compacted_at;

    return write_file_atomic(path, root.dump(), error);
}

bool validate_storage_backend(const Config& config, SyncState& state, std::string& error)
{
    // The custom store is the only supported backend. Keep the startup save so
    // sync.json always exists with defaults, but do not reject old directories.
    return save_sync_state(config.state_dir() / "sync.json", state, error);
}

bool sync_repository(const Config& config, const SyncCallbacks& callbacks, bool& stopped_flag, std::string& error)
{
    stopped_flag = false;
    const auto state_path = config.state_dir() / "sync.json";
    const auto metadata_path = config.state_dir() / "repository.json";
    const auto cache = cache_paths(config);

    if (!mkdir_p(config.data_dir(), error) || !mkdir_p(config.state_dir(), error)
        || !mkdir_p(cache.updates, error) || !mkdir_p(cache.incoming, error))
    {
        return false;
    }

    SyncState state;
    if (!load_sync_state(state_path, state, error))
    {
        return false;
    }

    if (!validate_storage_backend(config, state, error))
    {
        return false;
    }

    const auto manifest_path = config.store_dir() / "manifest.json";
    if (state.built && !path_exists(manifest_path))
    {
        HYD_LOG(warn, "sync state says the store is built, but manifest.json is missing; rebuilding from scratch");
        state.built = false;
        state.next_index = 0;
        state.delta_generation = 0;
        state.partial = false;
    }

    if (state.built)
    {
        StoreManifest existing;
        if (!load_store_manifest(config.store_dir(), existing, error))
        {
            return false;
        }
        if (existing.partitions != static_cast<std::uint32_t>(config.partitions()))
        {
            error = "the partitions setting cannot change after the store is built (store uses "
                + std::to_string(existing.partitions) + ", config uses "
                + std::to_string(config.partitions()) + "); rebuild the store or restore the old value";
            return false;
        }
    }

    RepositoryClient repository(config.repository_url(), config.repository_access_key());
    repository.set_timeout_seconds(config.get_int("download_timeout_seconds", 120));
    repository.set_rate_limit_kib(static_cast<std::uint64_t>(std::max<std::int64_t>(0, config.get_int("download_rate_limit_kib", 0))));

    RepositoryMetadata metadata;
    if (!load_repository_metadata_file(metadata_path, metadata, error))
    {
        return false;
    }

    const std::int64_t max_index = config.get_int("max_update_index", -1);
    if (max_index >= 0 && state.next_index > max_index)
    {
        HYD_LOG(info, "sync is capped by max_update_index; nothing to do");
        state.last_sync_at = now_unix();
        return save_sync_state(state_path, state, error);
    }

    const std::int64_t since = state.next_index;

    // A valid checkpoint already selected a consistent set of update files.
    // Finish that snapshot before asking the repository for newer indices.
    BuildCheckpoint checkpoint;
    const bool resume_snapshot = checkpoint_matches_metadata(config, state, metadata, max_index, checkpoint);
    if (resume_snapshot)
    {
        HYD_LOGF(info,
            "initial-build checkpoint is still valid; finishing its update snapshot "
            "before fetching new indices (%llu/%llu partitions complete)",
            static_cast<unsigned long long>(checkpoint.completed_partitions),
            static_cast<unsigned long long>(checkpoint.partitions));
    }
    else
    {
        HYD_LOGF(info, "checking repository metadata since index %lld", static_cast<long long>(since));
    }

    RepositoryMetadata fresh;
    if (!resume_snapshot)
    {
        if (!repository.fetch_metadata(since, fresh, error))
        {
            if (state.built)
            {
                HYD_LOGF(warn, "cannot fetch repository metadata, keeping the current store: %s", error.c_str());
                error.clear();
                return true;
            }
            return false;
        }

        metadata.merge(fresh);
        state.next_update_due = metadata.next_update_due;
        if (!save_repository_metadata_file(metadata_path, metadata, error))
        {
            return false;
        }
    }

    std::vector<UpdateSource> all_sources;
    std::int64_t highest_target = state.next_index - 1;

    for (const auto& [index, rows] : metadata.updates)
    {
        if (index < state.next_index)
        {
            continue;
        }
        if (max_index >= 0 && index > max_index)
        {
            continue;
        }

        highest_target = std::max(highest_target, index);
        for (const auto& update : rows)
        {
            UpdateSource source;
            source.update = update;
            source.cache_path = update_cache_path(cache, update.hash);
            all_sources.push_back(std::move(source));
        }
    }

    // Only sources for indices at or after state.next_index are relevant.
    if (!state.built)
    {
        // The initial build needs every index from zero. Rebuild the source
        // list from the merged metadata, including already-processed indices.
        all_sources.clear();
        highest_target = -1;
        for (const auto& [index, rows] : metadata.updates)
        {
            if (max_index >= 0 && index > max_index)
            {
                continue;
            }
            highest_target = std::max(highest_target, index);
            for (const auto& update : rows)
            {
                UpdateSource source;
                source.update = update;
                source.cache_path = update_cache_path(cache, update.hash);
                all_sources.push_back(std::move(source));
            }
        }
    }

    state.last_sync_started = now_unix();
    if (!save_sync_state(state_path, state, error))
    {
        return false;
    }

    if (!download_updates(config, all_sources, callbacks, state, repository, error))
    {
        if (error == "stopped")
        {
            stopped_flag = true;
            error.clear();
            return true;
        }
        return false;
    }

    // Every source must now be cached.
    for (const auto& source : all_sources)
    {
        if (!path_exists(source.cache_path) || file_size_or_zero(source.cache_path) == 0)
        {
            error = "update file " + hex_encode(source.update.hash) + " is missing from the cache";
            return false;
        }
    }

    if (!state.built)
    {
        if (!build_initial_store(config, metadata, all_sources, callbacks, state, error))
        {
            if (error == "stopped")
            {
                stopped_flag = true;
                error.clear();
                return true;
            }
            return false;
        }
    }
    else if (!all_sources.empty())
    {
        if (!apply_incremental(config, all_sources, callbacks, state, error))
        {
            if (error == "stopped")
            {
                stopped_flag = true;
                error.clear();
                return true;
            }
            return false;
        }
    }

    if (highest_target >= 0)
    {
        state.next_index = highest_target + 1;
    }

    if (resume_snapshot && state.built)
    {
        // The checkpoint covered an older, internally consistent snapshot.
        // Catch up with indices published since then. This is best-effort: the
        // store is already complete and consistent, and the next regular sync
        // retries if the repository is unavailable right now.
        RepositoryMetadata catchup;
        std::string catchup_error;
        if (!repository.fetch_metadata(state.next_index, catchup, catchup_error))
        {
            HYD_LOGF(warn, "initial build finished from the checkpointed snapshot; "
                "cannot fetch newer indices yet: %s", catchup_error.c_str());
        }
        else
        {
            metadata.merge(catchup);
            state.next_update_due = metadata.next_update_due;
            if (!save_repository_metadata_file(metadata_path, metadata, catchup_error))
            {
                HYD_LOGF(warn, "cannot save repository metadata after the initial build: %s",
                    catchup_error.c_str());
            }

            std::vector<UpdateSource> catchup_sources;
            std::int64_t catchup_target = state.next_index - 1;
            for (const auto& [index, rows] : metadata.updates)
            {
                if (index < state.next_index)
                {
                    continue;
                }
                if (max_index >= 0 && index > max_index)
                {
                    continue;
                }

                catchup_target = std::max(catchup_target, index);
                for (const auto& update : rows)
                {
                    UpdateSource source;
                    source.update = update;
                    source.cache_path = update_cache_path(cache, update.hash);
                    catchup_sources.push_back(std::move(source));
                }
            }

            if (!catchup_sources.empty())
            {
                HYD_LOGF(info, "catching up on %zu update files published since the checkpointed snapshot",
                    catchup_sources.size());
            }

            bool catchup_ok = true;
            if (!catchup_sources.empty())
            {
                catchup_ok = download_updates(
                    config, catchup_sources, callbacks, state, repository, catchup_error);
                if (!catchup_ok && catchup_error == "stopped")
                {
                    stopped_flag = true;
                    error.clear();
                    return true;
                }

                if (catchup_ok)
                {
                    for (const auto& source : catchup_sources)
                    {
                        if (!path_exists(source.cache_path)
                            || file_size_or_zero(source.cache_path) == 0)
                        {
                            catchup_error = "new update file " + hex_encode(source.update.hash)
                                + " is missing from the cache";
                            catchup_ok = false;
                            break;
                        }
                    }
                }

                if (!catchup_ok)
                {
                    HYD_LOGF(warn, "cannot apply newer updates yet: %s", catchup_error.c_str());
                }
                else if (!apply_incremental(config, catchup_sources, callbacks, state, catchup_error))
                {
                    if (catchup_error == "stopped")
                    {
                        stopped_flag = true;
                        error.clear();
                        return true;
                    }
                    HYD_LOGF(warn, "cannot apply newer updates yet: %s", catchup_error.c_str());
                    catchup_ok = false;
                }
            }

            if (catchup_ok && catchup_target >= 0)
            {
                highest_target = std::max(highest_target, catchup_target);
                state.next_index = highest_target + 1;
            }
        }
    }

    state.partial = (max_index >= 0 && state.next_index > max_index);

    state.last_sync_at = now_unix();
    if (!save_sync_state(state_path, state, error))
    {
        return false;
    }

    if (state.built)
    {
        std::error_code checkpoint_ec;
        std::filesystem::remove(config.store_dir() / "build.json", checkpoint_ec);
    }

    if (state.built && callbacks.on_store_ready)
    {
        callbacks.on_store_ready();
    }

    if (state.built && config.get_bool("compaction_enabled", true))
    {
        const auto threshold_mb = config.get_int("compact_after_delta_mb", 512);
        if (threshold_mb > 0
            && delta_bytes_on_disk(config) >= static_cast<std::uint64_t>(threshold_mb) * 1024 * 1024)
        {
            std::string compaction_error;
            if (!compact_store(config, callbacks, state, compaction_error))
            {
                if (compaction_error == "stopped")
                {
                    stopped_flag = true;
                    error.clear();
                    return true;
                }
                HYD_LOGF(warn, "automatic compaction failed, keeping the deltas: %s",
                    compaction_error.c_str());
            }
        }
    }

    if (config.get_bool("keep_update_cache", false) == false && state.built)
    {
        std::string cleanup_error;
        if (!clean_update_cache(config, cleanup_error))
        {
            HYD_LOGF(warn, "cannot clean update cache: %s", cleanup_error.c_str());
        }
    }

    HYD_LOGF(info, "sync complete: next index %lld, %llu hashes, %llu mappings, %llu tags",
        static_cast<long long>(state.next_index),
        static_cast<unsigned long long>(state.hash_count),
        static_cast<unsigned long long>(state.mapping_count),
        static_cast<unsigned long long>(state.tag_count));

    return true;
}


std::uint64_t delta_bytes_on_disk(const Config& config)
{
    const auto directory = config.store_dir() / "delta";
    std::uint64_t total = 0;
    std::error_code ec;
    if (!std::filesystem::exists(directory, ec))
    {
        return 0;
    }
    for (const auto& entry : std::filesystem::directory_iterator(directory, ec))
    {
        if (entry.is_regular_file())
        {
            total += file_size_or_zero(entry.path());
        }
    }
    return total;
}

bool compact_store(const Config& config, const SyncCallbacks& callbacks, SyncState& state, std::string& error)
{
    const auto store_dir = config.store_dir();
    const auto state_dir = config.state_dir();
    const auto partitions = static_cast<std::size_t>(config.partitions());

    DeltaData delta;
    bool truncated = false;
    if (!load_deltas(store_dir / "delta", partitions, delta, truncated, error))
    {
        return false;
    }
    if (delta.record_count == 0)
    {
        HYD_LOG(info, "compaction: no delta records to merge");
        return true;
    }

    HYD_LOGF(info, "compaction: merging %llu delta records (%s)",
        static_cast<unsigned long long>(delta.record_count),
        human_bytes(delta_bytes_on_disk(config)).c_str());

    StoreManifest manifest;
    if (!load_store_manifest(store_dir, manifest, error))
    {
        return false;
    }
    if (manifest.partitions != static_cast<std::uint32_t>(config.partitions()))
    {
        error = "cannot compact: the store uses " + std::to_string(manifest.partitions)
            + " partitions but the config uses " + std::to_string(config.partitions());
        return false;
    }

    std::vector<LegacyHashEntry> legacy_hashes;
    if (!read_legacy_hashes(store_dir / "legacy.bin", legacy_hashes, error))
    {
        return false;
    }
    legacy_hashes.insert(legacy_hashes.end(), delta.legacy_hashes.begin(), delta.legacy_hashes.end());
    if (!delta.deleted_files.empty())
    {
        legacy_hashes.erase(std::remove_if(legacy_hashes.begin(), legacy_hashes.end(),
                               [&delta](const LegacyHashEntry& entry) {
                                   return delta.deleted_files.count(entry.hash_id) != 0;
                               }),
            legacy_hashes.end());
    }

    // New tag definitions become a new append-only segment. Tag ids already
    // present in a manifested segment are ignored (this can happen after a
    // crash between writing the manifest and deleting deltas).
    std::vector<std::pair<std::uint32_t, std::string>> new_tags(delta.tags.begin(), delta.tags.end());
    std::sort(new_tags.begin(), new_tags.end(),
        [](const auto& left, const auto& right) { return left.first < right.first; });

    std::uint64_t max_existing_tag_id = 0;
    bool have_existing_tags = false;
    for (const auto& segment : manifest.tag_segments)
    {
        if (segment.count > 0)
        {
            max_existing_tag_id = std::max(max_existing_tag_id, segment.last);
            have_existing_tags = true;
        }
    }

    new_tags.erase(std::remove_if(new_tags.begin(), new_tags.end(),
                      [have_existing_tags, max_existing_tag_id](const auto& row) {
                          return have_existing_tags && row.first <= max_existing_tag_id;
                      }),
        new_tags.end());

    if (!new_tags.empty())
    {
        TagSegmentWriter writer;
        if (!writer.open(store_dir / "tags", manifest.tag_segments.size()))
        {
            error = writer.error;
            return false;
        }

        for (const auto& [tag_id, text] : new_tags)
        {
            if (!writer.add(tag_id, text))
            {
                error = writer.error;
                return false;
            }
        }

        if (!writer.finish(error))
        {
            return false;
        }

        char idx_name[64];
        char str_name[64];
        char ids_name[64];
        std::snprintf(idx_name, sizeof(idx_name), "tags/seg-%06zu.idx", manifest.tag_segments.size());
        std::snprintf(str_name, sizeof(str_name), "tags/seg-%06zu.str", manifest.tag_segments.size());
        std::snprintf(ids_name, sizeof(ids_name), "tags/seg-%06zu.ids", manifest.tag_segments.size());
        manifest.tag_segments.push_back(StoreManifest::TagSegmentInfo {
            writer.base_id,
            writer.count,
            writer.last_id,
            idx_name,
            str_name,
            ids_name,
        });
        manifest.tag_count += writer.count;
    }

    // Merge tag pairs.
    {
        std::unordered_map<std::uint32_t, std::vector<std::uint32_t>> siblings;
        if (!read_pairs(store_dir / "siblings.bin", siblings, error))
        {
            return false;
        }
        for (const auto& [bad, good] : delta.sibling_additions)
        {
            siblings[bad] = { good };
        }
        for (const auto& key : delta.sibling_deletions)
        {
            const auto bad = static_cast<std::uint32_t>(key >> 32);
            const auto good = static_cast<std::uint32_t>(key & 0xffffffffu);
            auto iterator = siblings.find(bad);
            if (iterator != siblings.end())
            {
                iterator->second.erase(
                    std::remove(iterator->second.begin(), iterator->second.end(), good),
                    iterator->second.end());
                if (iterator->second.empty())
                {
                    siblings.erase(iterator);
                }
            }
        }
        if (!write_pairs(store_dir / "siblings.bin", siblings, error))
        {
            return false;
        }
    }

    {
        std::unordered_map<std::uint32_t, std::vector<std::uint32_t>> parents;
        if (!read_pairs(store_dir / "parents.bin", parents, error))
        {
            return false;
        }
        for (const auto& [child, ancestors] : delta.parent_additions)
        {
            for (const auto parent : ancestors)
            {
                auto& list = parents[child];
                if (std::find(list.begin(), list.end(), parent) == list.end())
                {
                    list.push_back(parent);
                }
            }
        }
        for (const auto& key : delta.parent_deletions)
        {
            const auto child = static_cast<std::uint32_t>(key >> 32);
            const auto parent = static_cast<std::uint32_t>(key & 0xffffffffu);
            auto iterator = parents.find(child);
            if (iterator != parents.end())
            {
                iterator->second.erase(
                    std::remove(iterator->second.begin(), iterator->second.end(), parent),
                    iterator->second.end());
                if (iterator->second.empty())
                {
                    parents.erase(iterator);
                }
            }
        }
        if (!write_pairs(store_dir / "parents.bin", parents, error))
        {
            return false;
        }
    }

    std::uint64_t hash_count = 0;
    std::uint64_t mapping_count = 0;

    for (std::size_t p = 0; p < partitions; ++p)
    {
        if (stopped(callbacks))
        {
            error = "stopped";
            return false;
        }

        char name[32];
        std::snprintf(name, sizeof(name), "p%02zu", p);
        const auto partition_dir = store_dir / name;

        std::vector<HashEntry> hashes;
        std::unordered_map<std::uint32_t, std::vector<std::uint32_t>> mappings;
        if (!read_base_partition(partition_dir, hashes, mappings, error))
        {
            return false;
        }

        std::unordered_set<std::uint32_t> ids;
        ids.reserve(hashes.size() + legacy_hashes.size() + 1024);
        for (const auto& entry : hashes)
        {
            ids.insert(entry.hash_id);
        }
        for (const auto& entry : legacy_hashes)
        {
            if (static_cast<std::size_t>(entry.hash[0]) * partitions / 256 == p)
            {
                ids.insert(entry.hash_id);
            }
        }

        if (p < delta.hashes_by_partition.size())
        {
            for (const auto& entry : delta.hashes_by_partition[p])
            {
                if (ids.insert(entry.hash_id).second)
                {
                    hashes.push_back(entry);
                }
            }
        }

        // File deletions win over any other record.
        if (!delta.deleted_files.empty())
        {
            hashes.erase(std::remove_if(hashes.begin(), hashes.end(),
                             [&delta](const HashEntry& entry) {
                                 return delta.deleted_files.count(entry.hash_id) != 0;
                             }),
                hashes.end());
            for (auto hash_id : delta.deleted_files)
            {
                mappings.erase(hash_id);
                ids.erase(hash_id);
            }
        }

        for (const auto& [hash_id, tag_ids] : delta.mapping_additions)
        {
            if (ids.count(hash_id) == 0)
            {
                continue;
            }
            auto& list = mappings[hash_id];
            for (const auto tag_id : tag_ids)
            {
                add_mapping(list, tag_id);
            }
        }

        for (const auto& key : delta.mapping_deletions)
        {
            const auto hash_id = static_cast<std::uint32_t>(key >> 32);
            if (ids.count(hash_id) == 0)
            {
                continue;
            }
            const auto tag_id = static_cast<std::uint32_t>(key & 0xffffffffu);
            const auto iterator = mappings.find(hash_id);
            if (iterator != mappings.end())
            {
                remove_mapping(iterator->second, tag_id);
            }
        }

        for (auto& [hash_id, tag_ids] : mappings)
        {
            (void)hash_id;
            std::sort(tag_ids.begin(), tag_ids.end());
            tag_ids.erase(std::unique(tag_ids.begin(), tag_ids.end()), tag_ids.end());
            mapping_count += tag_ids.size();
        }

        hash_count += hashes.size();

        if (!write_partition_hashes(partition_dir / "hashes.bin", hashes, error))
        {
            return false;
        }
        if (!write_partition_maps(partition_dir / "maps.bin", mappings, error))
        {
            return false;
        }

        HYD_LOGF(debug, "compaction: partition %zu/%zu merged", p + 1, partitions);
    }

    if (!write_legacy_hashes(store_dir / "legacy.bin", legacy_hashes, error))
    {
        return false;
    }

    hash_count += legacy_hashes.size();
    manifest.hash_count = hash_count;
    manifest.mapping_count = mapping_count;
    if (!save_store_manifest(store_dir, manifest, error))
    {
        return false;
    }

    // Remove merged delta files only after the new manifest is durable.
    {
        std::error_code ec;
        const auto delta_dir = store_dir / "delta";
        for (const auto& entry : std::filesystem::directory_iterator(delta_dir, ec))
        {
            if (entry.is_regular_file() && ends_with(entry.path().filename().string(), ".bin"))
            {
                std::filesystem::remove(entry.path(), ec);
                if (ec)
                {
                    error = "cannot remove merged delta " + entry.path().string() + ": " + ec.message();
                    return false;
                }
            }
        }
        if (!fsync_directory(delta_dir, error))
        {
            return false;
        }
    }

    // Remove orphaned tag segments from interrupted compactions.
    {
        std::error_code ec;
        std::unordered_set<std::string> referenced;
        for (const auto& segment : manifest.tag_segments)
        {
            referenced.insert(std::filesystem::path(segment.idx_path).filename().string());
            referenced.insert(std::filesystem::path(segment.str_path).filename().string());
            referenced.insert(std::filesystem::path(segment.ids_path).filename().string());
        }
        for (const auto& entry : std::filesystem::directory_iterator(store_dir / "tags", ec))
        {
            const auto name = entry.path().filename().string();
            if (entry.is_regular_file() && starts_with(name, "seg-") && referenced.count(name) == 0)
            {
                std::filesystem::remove(entry.path(), ec);
            }
        }
    }

    state.hash_count = manifest.hash_count;
    state.mapping_count = manifest.mapping_count;
    state.tag_count = manifest.tag_count;
    state.last_compacted_at = now_unix();
    if (!save_sync_state(state_dir / "sync.json", state, error))
    {
        return false;
    }

    HYD_LOGF(info, "compaction complete: %llu hashes, %llu mappings, %llu tags",
        static_cast<unsigned long long>(manifest.hash_count),
        static_cast<unsigned long long>(manifest.mapping_count),
        static_cast<unsigned long long>(manifest.tag_count));

    if (callbacks.on_store_ready)
    {
        callbacks.on_store_ready();
    }

    return true;
}


namespace {

std::filesystem::path cached_update_path(const Config& config, const Hash& hash)
{
    return config.cache_dir() / "updates" / (hex_encode(hash) + ".bin");
}

bool download_repairs(const Config& config, const std::vector<DownloadItem>& items,
    const SyncCallbacks& callbacks, CacheRepairResult& result, std::string& error)
{
    const auto summary = download_update_files(config, items, callbacks.should_stop);
    if (!summary.error.empty())
    {
        error = summary.error;
        return false;
    }
    if (summary.stopped)
    {
        error = "stopped";
        return false;
    }

    for (const auto& item : items)
    {
        if (!path_exists(item.cache_path) || file_size_or_zero(item.cache_path) == 0)
        {
            ++result.failed;
            continue;
        }

        std::string read_error;
        auto bytes = read_file(item.cache_path, read_error);
        if (bytes && sha256(*bytes) == item.hash)
        {
            ++result.repaired;
        }
        else
        {
            ++result.failed;
        }
    }

    if (result.failed > 0)
    {
        error = std::to_string(result.failed) + " update files could not be repaired";
        return false;
    }
    return true;
}

} // namespace

bool repair_update_cache(const Config& config, const SyncCallbacks& callbacks,
    CacheRepairResult& result, std::string& error)
{
    result = CacheRepairResult {};

    if (!config.get_bool("keep_update_cache", false))
    {
        error = "update cache is disabled; set keep_update_cache = true to repair it";
        return false;
    }

    RepositoryMetadata metadata;
    if (!load_repository_metadata_file(config.state_dir() / "repository.json", metadata, error))
    {
        return false;
    }

    std::vector<DownloadItem> items;
    std::unordered_set<std::string> seen;
    for (const auto& [index, rows] : metadata.updates)
    {
        (void)index;
        for (const auto& update : rows)
        {
            const std::string key = hex_encode(update.hash);
            if (!seen.insert(key).second)
            {
                continue;
            }

            ++result.checked;
            const auto path = cached_update_path(config, update.hash);
            bool needs_download = false;

            if (!path_exists(path) || file_size_or_zero(path) == 0)
            {
                ++result.missing;
                needs_download = true;
            }
            else
            {
                std::string read_error;
                auto bytes = read_file(path, read_error);
                if (!bytes)
                {
                    ++result.corrupt;
                    needs_download = true;
                }
                else if (sha256(*bytes) != update.hash)
                {
                    HYD_LOGF(warn, "repair: cached update %s is corrupt", key.c_str());
                    std::error_code remove_ec;
                    std::filesystem::remove(path, remove_ec);
                    ++result.corrupt;
                    needs_download = true;
                }
            }

            if (needs_download)
            {
                items.push_back(DownloadItem { update.hash, path });
            }
        }
    }

    if (result.checked == 0)
    {
        error = "repository metadata lists no update files";
        return false;
    }

    if (items.empty())
    {
        HYD_LOGF(info, "update cache verified: %llu files intact",
            static_cast<unsigned long long>(result.checked));
        return true;
    }

    HYD_LOGF(warn, "update cache needs repair: %llu missing, %llu corrupt of %llu files",
        static_cast<unsigned long long>(result.missing),
        static_cast<unsigned long long>(result.corrupt),
        static_cast<unsigned long long>(result.checked));

    if (!download_repairs(config, items, callbacks, result, error))
    {
        return false;
    }

    HYD_LOGF(info, "update cache repaired: %llu files downloaded",
        static_cast<unsigned long long>(result.repaired));
    return true;
}

bool repair_update_files(const Config& config, const std::vector<Hash>& hashes,
    const SyncCallbacks& callbacks, CacheRepairResult& result, std::string& error)
{
    result = CacheRepairResult {};

    if (!config.get_bool("keep_update_cache", false))
    {
        error = "update cache is disabled; set keep_update_cache = true to repair it";
        return false;
    }

    std::vector<DownloadItem> items;
    std::unordered_set<std::string> seen;
    for (const auto& hash : hashes)
    {
        const std::string key = hex_encode(hash);
        if (!seen.insert(key).second)
        {
            continue;
        }

        ++result.checked;
        const auto path = cached_update_path(config, hash);
        bool needs_download = false;

        if (!path_exists(path) || file_size_or_zero(path) == 0)
        {
            ++result.missing;
            needs_download = true;
        }
        else
        {
            std::string read_error;
            auto bytes = read_file(path, read_error);
            if (!bytes || sha256(*bytes) != hash)
            {
                std::error_code remove_ec;
                std::filesystem::remove(path, remove_ec);
                ++result.corrupt;
                needs_download = true;
            }
        }

        if (needs_download)
        {
            items.push_back(DownloadItem { hash, path });
        }
    }

    if (items.empty())
    {
        return true;
    }

    HYD_LOGF(info, "repairing %zu cached update files", items.size());
    if (!download_repairs(config, items, callbacks, result, error))
    {
        return false;
    }

    HYD_LOGF(info, "repaired %llu cached update files",
        static_cast<unsigned long long>(result.repaired));
    return true;
}

bool clean_update_cache(const Config& config, std::string& error)
{
    const auto cache = cache_paths(config);
    std::error_code ec;
    for (const auto& directory : { cache.updates, cache.incoming })
    {
        if (!std::filesystem::exists(directory, ec))
        {
            continue;
        }
        for (const auto& entry : std::filesystem::directory_iterator(directory, ec))
        {
            if (entry.is_regular_file())
            {
                std::filesystem::remove(entry.path(), ec);
                if (ec)
                {
                    error = "cannot remove " + entry.path().string() + ": " + ec.message();
                    return false;
                }
            }
        }
    }
    return true;
}

} // namespace hyd
