#include "store/builder.hpp"

#include "common/log.hpp"
#include "common/util.hpp"
#include "hydrus/update.hpp"
#include "store/format.hpp"
#include "store/store.hpp"
#include "store/sqlite_sync.hpp"
#include "store/delta.hpp"

#include <nlohmann/json.hpp>
#include <zlib.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <map>
#include <fstream>
#include <numeric>
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

struct BuildStats
{
    std::uint64_t hash_count = 0;
    std::uint64_t mapping_count = 0;
    std::uint64_t tag_count = 0;
    std::uint32_t tag_base = 0;
};

class DefinitionVisitor : public UpdateVisitor
{
public:
    DefinitionVisitor(TagSegmentWriter& tags, std::vector<std::ofstream>& hash_writers, std::size_t partitions,
        std::uint64_t& max_hash_id)
        : tags_(tags), hash_writers_(hash_writers), partitions_(partitions), max_hash_id_(max_hash_id)
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

    std::error_code ec;
    std::filesystem::remove_all(store_dir, ec);
    if (ec)
    {
        error = "cannot clear old store: " + ec.message();
        return false;
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

    HYD_LOGF(info, "initial sync: building tag dictionary and hash definitions from %zu update files",
        sources.size());

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

    std::uint64_t max_hash_id = 0;
    DefinitionVisitor definition_visitor(tag_writer, hash_writers, partitions, max_hash_id);
    UpdateParser parser;
    std::unordered_map<std::string, int> kinds;

    std::size_t processed = 0;
    auto last_report = std::chrono::steady_clock::now();

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
        if (!parser.parse(*bytes, definition_visitor, kind, error))
        {
            error = "update " + hex_encode(source.update.hash) + ": " + error;
            return false;
        }
        kinds[hex_encode(source.update.hash)] = static_cast<int>(kind);
        ++processed;

        const auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration<double>(now - last_report).count() > 10.0)
        {
            HYD_LOGF(info, "definitions: %zu/%zu update files", processed, sources.size());
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

    if (!tag_writer.finish(error))
    {
        return false;
    }

    if (!save_update_kinds(state_dir / "update_kinds.json", kinds, error))
    {
        return false;
    }

    BuildStats stats;
    stats.tag_count = tag_writer.count;
    stats.tag_base = static_cast<std::uint32_t>(tag_writer.base_id);

    std::unordered_map<std::uint32_t, std::uint32_t> siblings;
    std::unordered_map<std::uint32_t, std::vector<std::uint32_t>> parents;

    // Phase B: one pass per partition over all content updates.
    for (std::size_t p = 0; p < partitions; ++p)
    {
        if (stopped(callbacks))
        {
            error = "stopped";
            return false;
        }

        HYD_LOGF(info, "partition %zu/%zu: reading hash definitions", p + 1, partitions);

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

        std::unordered_map<std::uint32_t, std::vector<std::uint32_t>> mappings;
        mappings.reserve(hash_entries.size());
        std::unordered_set<std::uint32_t> deleted_hashes;

        PartitionVisitor partition_visitor(p, partitions, membership, mappings, deleted_hashes,
            p == 0, siblings, parents);

        std::size_t content_files = 0;
        for (const auto& source : sources)
        {
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
            }
        }

        for (auto hash_id : deleted_hashes)
        {
            hash_entries.erase(std::remove_if(hash_entries.begin(), hash_entries.end(),
                                   [hash_id](const HashEntry& entry) { return entry.hash_id == hash_id; }),
                hash_entries.end());
        }

        HYD_LOGF(info, "partition %zu/%zu: %zu hashes, %zu mappings (%zu content updates)",
            p + 1, partitions, hash_entries.size(), mappings.size(), content_files);

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

        std::error_code remove_ec;
        std::filesystem::remove(defs_path, remove_ec);

        hash_entries.clear();
        hash_entries.shrink_to_fit();
        mappings.clear();
        membership.clear();
        membership.shrink_to_fit();
    }

    if (!write_pairs(store_dir / "siblings.bin", siblings, error))
    {
        return false;
    }
    if (!write_pairs(store_dir / "parents.bin", parents, error))
    {
        return false;
    }

    StoreManifest manifest;
    manifest.format = kStoreFormatVersion;
    manifest.partitions = static_cast<std::uint32_t>(partitions);
    manifest.hash_count = stats.hash_count;
    manifest.mapping_count = stats.mapping_count;
    manifest.tag_count = stats.tag_count;
    manifest.built_at = now_unix();
    manifest.tag_segments.push_back(StoreManifest::TagSegmentInfo {
        tag_writer.base_id,
        tag_writer.count,
        tag_writer.last_id,
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
    const auto limit = config.max_download_bytes();
    std::uint64_t downloaded_this_run = 0;
    std::size_t processed = 0;
    auto last_report = std::chrono::steady_clock::now();

    for (const auto& source : sources)
    {
        if (stopped(callbacks))
        {
            error = "stopped";
            return false;
        }

        if (path_exists(source.cache_path) && file_size_or_zero(source.cache_path) > 0)
        {
            continue;
        }

        HYD_LOGF(debug, "downloading update %s", hex_encode(source.update.hash).c_str());

        Bytes body;
        if (!repository.fetch_update(source.update.hash, body, error))
        {
            return false;
        }

        if (limit != 0 && state.downloaded_bytes + downloaded_this_run + body.size() > limit)
        {
            error = "download limit of " + human_bytes(limit) + " reached; refusing to download more update files";
            return false;
        }

        if (!write_file_atomic(source.cache_path, body, error))
        {
            return false;
        }

        downloaded_this_run += body.size();
        state.downloaded_bytes += body.size();
        ++state.downloaded_files;
        ++processed;

        const auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration<double>(now - last_report).count() > 10.0)
        {
            HYD_LOGF(info, "downloaded %zu update files this run (%s total this run)",
                processed, human_bytes(downloaded_this_run).c_str());
            last_report = now;
        }
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
        state.downloaded_bytes = root.value("downloaded_bytes", static_cast<std::uint64_t>(0));
        state.downloaded_files = root.value("downloaded_files", static_cast<std::uint64_t>(0));
        state.delta_generation = root.value("delta_generation", 0u);
        state.hash_count = root.value("hash_count", static_cast<std::uint64_t>(0));
        state.mapping_count = root.value("mapping_count", static_cast<std::uint64_t>(0));
        state.tag_count = root.value("tag_count", static_cast<std::uint64_t>(0));
        state.last_compacted_at = root.value("last_compacted_at", static_cast<std::int64_t>(0));
        state.backend = root.value("backend", std::string());
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
    root["downloaded_bytes"] = state.downloaded_bytes;
    root["downloaded_files"] = state.downloaded_files;
    root["delta_generation"] = state.delta_generation;
    root["hash_count"] = state.hash_count;
    root["mapping_count"] = state.mapping_count;
    root["tag_count"] = state.tag_count;
    root["last_compacted_at"] = state.last_compacted_at;
    root["backend"] = state.backend;

    return write_file_atomic(path, root.dump(), error);
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

    const bool compression = config.get_bool("compression", true);
    const std::string desired_backend = compression ? "custom" : "sqlite";
    if (state.backend.empty())
    {
        if (path_exists(config.store_dir() / "manifest.json"))
        {
            state.backend = "custom";
        }
        else if (path_exists(sqlite_store_path(config)))
        {
            state.backend = "sqlite";
        }
        else
        {
            state.backend = desired_backend;
        }
    }
    if (state.backend != desired_backend)
    {
        error = "this data directory uses the '" + state.backend + "' backend, but compression = "
            + (compression ? "true" : "false") + " selects the '" + desired_backend
            + "' backend; use a new data directory or set compression back";
        return false;
    }

    if (!compression)
    {
        return sync_sqlite(config, callbacks, stopped_flag, state, error);
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
    HYD_LOGF(info, "checking repository metadata since index %lld", static_cast<long long>(since));

    RepositoryMetadata fresh;
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
    if (!save_repository_metadata_file(metadata_path, metadata, error))
    {
        return false;
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

    state.partial = (max_index >= 0 && state.next_index > max_index);

    state.last_sync_at = now_unix();
    if (!save_sync_state(state_path, state, error))
    {
        return false;
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
        ids.reserve(hashes.size() + 1024);
        for (const auto& entry : hashes)
        {
            ids.insert(entry.hash_id);
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
