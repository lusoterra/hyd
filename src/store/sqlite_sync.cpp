#include "store/sqlite_sync.hpp"

#include "common/log.hpp"
#include "common/util.hpp"
#include "hydrus/repository.hpp"
#include "hydrus/update.hpp"
#include "store/builder.hpp"

#include <nlohmann/json.hpp>
#include <sqlite3.h>

#include <algorithm>
#include <map>
#include <unordered_map>

namespace hyd {

namespace {

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

    class NoopVisitor : public UpdateVisitor
    {
    };

    NoopVisitor visitor;
    if (!parser.parse(compressed, visitor, kind, error))
    {
        return false;
    }
    cache[key] = static_cast<int>(kind);
    return true;
}

bool download_updates(const Config& config, const std::vector<UpdateSource>& sources,
    const SyncCallbacks& callbacks, SyncState& state, RepositoryClient& repository, std::string& error)
{
    const auto limit = config.max_download_bytes();
    std::uint64_t downloaded_this_run = 0;
    std::size_t processed = 0;

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

        if (processed % 100 == 0)
        {
            HYD_LOGF(info, "downloaded %zu update files this run (%s)",
                processed, human_bytes(downloaded_this_run).c_str());
        }
    }

    return true;
}

bool sqlite_exec(sqlite3* db, const char* sql, std::string& error)
{
    char* message = nullptr;
    const int result = sqlite3_exec(db, sql, nullptr, nullptr, &message);
    if (result != SQLITE_OK)
    {
        error = message != nullptr ? message : sqlite3_errstr(result);
        sqlite3_free(message);
        return false;
    }
    return true;
}

class SqliteImporter
{
public:
    ~SqliteImporter()
    {
        for (auto* statement : statements_)
        {
            if (statement != nullptr)
            {
                sqlite3_finalize(statement);
            }
        }
        if (db_ != nullptr)
        {
            sqlite3_close(db_);
        }
    }

    bool open(const std::filesystem::path& path, std::string& error)
    {
        if (!mkdir_p(path.parent_path(), error))
        {
            return false;
        }

        if (sqlite3_open_v2(path.c_str(), &db_,
                SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX, nullptr) != SQLITE_OK)
        {
            error = "cannot open SQLite store " + path.string() + ": "
                + (db_ != nullptr ? sqlite3_errmsg(db_) : "unknown error");
            return false;
        }

        sqlite3_busy_timeout(db_, 10000);

        const char* pragmas[] = {
            "PRAGMA journal_mode=WAL",
            "PRAGMA synchronous=NORMAL",
            "PRAGMA cache_size=-524288",
            "PRAGMA temp_store=MEMORY",
            "PRAGMA foreign_keys=OFF",
        };
        for (const char* pragma : pragmas)
        {
            if (!sqlite_exec(db_, pragma, error))
            {
                return false;
            }
        }

        const char* schema[] = {
            "CREATE TABLE IF NOT EXISTS meta(key TEXT PRIMARY KEY, value INTEGER NOT NULL)",
            "CREATE TABLE IF NOT EXISTS hashes(hash_id INTEGER PRIMARY KEY, hash BLOB NOT NULL UNIQUE)",
            "CREATE TABLE IF NOT EXISTS tags(tag_id INTEGER PRIMARY KEY, tag TEXT NOT NULL)",
            "CREATE TABLE IF NOT EXISTS mappings(hash_id INTEGER NOT NULL, tag_id INTEGER NOT NULL, PRIMARY KEY(hash_id, tag_id)) WITHOUT ROWID",
            "CREATE TABLE IF NOT EXISTS siblings(bad_tag_id INTEGER PRIMARY KEY, good_tag_id INTEGER NOT NULL) WITHOUT ROWID",
            "CREATE TABLE IF NOT EXISTS parents(child_tag_id INTEGER NOT NULL, parent_tag_id INTEGER NOT NULL, PRIMARY KEY(child_tag_id, parent_tag_id)) WITHOUT ROWID",
        };
        for (const char* statement : schema)
        {
            if (!sqlite_exec(db_, statement, error))
            {
                return false;
            }
        }

        const char* queries[] = {
            "INSERT OR REPLACE INTO hashes(hash_id, hash) VALUES(?1, ?2)",
            "INSERT OR REPLACE INTO tags(tag_id, tag) VALUES(?1, ?2)",
            "INSERT OR IGNORE INTO mappings(hash_id, tag_id) VALUES(?1, ?2)",
            "DELETE FROM mappings WHERE hash_id = ?1 AND tag_id = ?2",
            "DELETE FROM mappings WHERE hash_id = ?1",
            "DELETE FROM hashes WHERE hash_id = ?1",
            "INSERT OR REPLACE INTO siblings(bad_tag_id, good_tag_id) VALUES(?1, ?2)",
            "DELETE FROM siblings WHERE bad_tag_id = ?1 AND good_tag_id = ?2",
            "INSERT OR IGNORE INTO parents(child_tag_id, parent_tag_id) VALUES(?1, ?2)",
            "DELETE FROM parents WHERE child_tag_id = ?1 AND parent_tag_id = ?2",
            "INSERT INTO meta(key, value) VALUES(?1, ?2) ON CONFLICT(key) DO UPDATE SET value = excluded.value",
        };

        for (std::size_t i = 0; i < std::size(queries); ++i)
        {
            if (sqlite3_prepare_v2(db_, queries[i], -1, &statements_[i], nullptr) != SQLITE_OK)
            {
                error = std::string("cannot prepare SQLite statement: ") + sqlite3_errmsg(db_);
                return false;
            }
        }

        sqlite3_stmt* counter = nullptr;
        if (sqlite3_prepare_v2(db_, "SELECT value FROM meta WHERE key = ?1", -1, &counter, nullptr) != SQLITE_OK)
        {
            error = std::string("cannot prepare SQLite metadata query: ") + sqlite3_errmsg(db_);
            return false;
        }

        auto read_count = [&](const char* key) -> std::uint64_t {
            sqlite3_reset(counter);
            sqlite3_bind_text(counter, 1, key, -1, SQLITE_STATIC);
            if (sqlite3_step(counter) == SQLITE_ROW)
            {
                return static_cast<std::uint64_t>(sqlite3_column_int64(counter, 0));
            }
            return 0;
        };

        hash_count_ = read_count("hash_count");
        mapping_count_ = read_count("mapping_count");
        tag_count_ = read_count("tag_count");
        sqlite3_finalize(counter);

        return true;
    }

    bool begin(std::string& error) { return sqlite_exec(db_, "BEGIN IMMEDIATE", error); }
    bool commit(std::string& error) { return sqlite_exec(db_, "COMMIT", error); }
    bool rollback()
    {
        std::string ignored;
        return sqlite_exec(db_, "ROLLBACK", ignored);
    }
    bool checkpoint(std::string& error) { return sqlite_exec(db_, "PRAGMA wal_checkpoint(PASSIVE)", error); }

    void add_hash(std::uint32_t hash_id, const Hash& hash)
    {
        sqlite3_reset(statements_[ins_hash]);
        sqlite3_bind_int64(statements_[ins_hash], 1, static_cast<sqlite3_int64>(hash_id));
        sqlite3_bind_blob(statements_[ins_hash], 2, hash.data(), static_cast<int>(hash.size()), SQLITE_TRANSIENT);
        step(statements_[ins_hash]);
        if (sqlite3_changes(db_) > 0)
        {
            ++hash_count_;
        }
    }

    void add_tag(std::uint32_t tag_id, std::string_view tag)
    {
        sqlite3_reset(statements_[ins_tag]);
        sqlite3_bind_int64(statements_[ins_tag], 1, static_cast<sqlite3_int64>(tag_id));
        sqlite3_bind_text(statements_[ins_tag], 2, tag.data(), static_cast<int>(tag.size()), SQLITE_TRANSIENT);
        step(statements_[ins_tag]);
        if (sqlite3_changes(db_) > 0)
        {
            ++tag_count_;
        }
    }

    void add_mapping(std::uint32_t hash_id, std::uint32_t tag_id)
    {
        sqlite3_reset(statements_[ins_mapping]);
        sqlite3_bind_int64(statements_[ins_mapping], 1, static_cast<sqlite3_int64>(hash_id));
        sqlite3_bind_int64(statements_[ins_mapping], 2, static_cast<sqlite3_int64>(tag_id));
        step(statements_[ins_mapping]);
        if (sqlite3_changes(db_) > 0)
        {
            ++mapping_count_;
        }
    }

    void delete_mapping(std::uint32_t hash_id, std::uint32_t tag_id)
    {
        sqlite3_reset(statements_[del_mapping]);
        sqlite3_bind_int64(statements_[del_mapping], 1, static_cast<sqlite3_int64>(hash_id));
        sqlite3_bind_int64(statements_[del_mapping], 2, static_cast<sqlite3_int64>(tag_id));
        step(statements_[del_mapping]);
        const auto removed = static_cast<std::uint64_t>(sqlite3_changes(db_));
        mapping_count_ = mapping_count_ > removed ? mapping_count_ - removed : 0;
    }

    void delete_file(std::uint32_t hash_id)
    {
        sqlite3_reset(statements_[del_file_mappings]);
        sqlite3_bind_int64(statements_[del_file_mappings], 1, static_cast<sqlite3_int64>(hash_id));
        step(statements_[del_file_mappings]);
        const auto removed = static_cast<std::uint64_t>(sqlite3_changes(db_));
        mapping_count_ = mapping_count_ > removed ? mapping_count_ - removed : 0;

        sqlite3_reset(statements_[del_file]);
        sqlite3_bind_int64(statements_[del_file], 1, static_cast<sqlite3_int64>(hash_id));
        step(statements_[del_file]);
        if (sqlite3_changes(db_) > 0 && hash_count_ > 0)
        {
            --hash_count_;
        }
    }

    void add_sibling(std::uint32_t bad, std::uint32_t good)
    {
        sqlite3_reset(statements_[ins_sibling]);
        sqlite3_bind_int64(statements_[ins_sibling], 1, static_cast<sqlite3_int64>(bad));
        sqlite3_bind_int64(statements_[ins_sibling], 2, static_cast<sqlite3_int64>(good));
        step(statements_[ins_sibling]);
    }

    void delete_sibling(std::uint32_t bad, std::uint32_t good)
    {
        sqlite3_reset(statements_[del_sibling]);
        sqlite3_bind_int64(statements_[del_sibling], 1, static_cast<sqlite3_int64>(bad));
        sqlite3_bind_int64(statements_[del_sibling], 2, static_cast<sqlite3_int64>(good));
        step(statements_[del_sibling]);
    }

    void add_parent(std::uint32_t child, std::uint32_t parent)
    {
        sqlite3_reset(statements_[ins_parent]);
        sqlite3_bind_int64(statements_[ins_parent], 1, static_cast<sqlite3_int64>(child));
        sqlite3_bind_int64(statements_[ins_parent], 2, static_cast<sqlite3_int64>(parent));
        step(statements_[ins_parent]);
    }

    void delete_parent(std::uint32_t child, std::uint32_t parent)
    {
        sqlite3_reset(statements_[del_parent]);
        sqlite3_bind_int64(statements_[del_parent], 1, static_cast<sqlite3_int64>(child));
        sqlite3_bind_int64(statements_[del_parent], 2, static_cast<sqlite3_int64>(parent));
        step(statements_[del_parent]);
    }

    bool store_counts(std::string& error)
    {
        (void)error;
        auto put = [&](const char* key, std::uint64_t value) -> bool {
            sqlite3_reset(statements_[set_meta]);
            sqlite3_bind_text(statements_[set_meta], 1, key, -1, SQLITE_STATIC);
            sqlite3_bind_int64(statements_[set_meta], 2, static_cast<sqlite3_int64>(value));
            return step(statements_[set_meta]);
        };

        return put("hash_count", hash_count_) && put("mapping_count", mapping_count_)
            && put("tag_count", tag_count_);
    }

    std::uint64_t hash_count() const { return hash_count_; }
    std::uint64_t mapping_count() const { return mapping_count_; }
    std::uint64_t tag_count() const { return tag_count_; }

private:
    enum Statement
    {
        ins_hash = 0,
        ins_tag,
        ins_mapping,
        del_mapping,
        del_file_mappings,
        del_file,
        ins_sibling,
        del_sibling,
        ins_parent,
        del_parent,
        set_meta,
        statement_count,
    };

    bool step(sqlite3_stmt* statement)
    {
        const int result = sqlite3_step(statement);
        if (result != SQLITE_DONE && result != SQLITE_ROW)
        {
            throw std::runtime_error(std::string("SQLite write failed: ") + sqlite3_errmsg(db_));
        }
        return true;
    }

    sqlite3* db_ = nullptr;
    sqlite3_stmt* statements_[statement_count] = {};
    std::uint64_t hash_count_ = 0;
    std::uint64_t mapping_count_ = 0;
    std::uint64_t tag_count_ = 0;
};

class SqliteDefinitionVisitor : public UpdateVisitor
{
public:
    explicit SqliteDefinitionVisitor(SqliteImporter& importer) : importer_(importer) {}

    void on_hash_definition(std::uint32_t hash_id, const Hash& hash) override
    {
        importer_.add_hash(hash_id, hash);
    }

    void on_tag_definition(std::uint32_t tag_id, std::string_view tag) override
    {
        importer_.add_tag(tag_id, tag);
    }

private:
    SqliteImporter& importer_;
};

class SqliteContentVisitor : public UpdateVisitor
{
public:
    explicit SqliteContentVisitor(SqliteImporter& importer) : importer_(importer) {}

    void on_mapping_add(std::uint32_t tag_id, std::uint32_t hash_id) override
    {
        importer_.add_mapping(hash_id, tag_id);
    }

    void on_mapping_delete(std::uint32_t tag_id, std::uint32_t hash_id) override
    {
        importer_.delete_mapping(hash_id, tag_id);
    }

    void on_file_delete(std::uint32_t hash_id) override { importer_.delete_file(hash_id); }
    void on_sibling_add(std::uint32_t bad, std::uint32_t good) override { importer_.add_sibling(bad, good); }
    void on_sibling_delete(std::uint32_t bad, std::uint32_t good) override { importer_.delete_sibling(bad, good); }
    void on_parent_add(std::uint32_t child, std::uint32_t parent) override { importer_.add_parent(child, parent); }
    void on_parent_delete(std::uint32_t child, std::uint32_t parent) override { importer_.delete_parent(child, parent); }

private:
    SqliteImporter& importer_;
};

} // namespace

std::filesystem::path sqlite_store_path(const Config& config)
{
    return config.data_dir() / "sqlite" / "hyd.sqlite3";
}

bool sync_sqlite(const Config& config, const SyncCallbacks& callbacks, bool& stopped_flag,
    SyncState& state, std::string& error)
{
    stopped_flag = false;
    const auto state_path = config.state_dir() / "sync.json";
    const auto metadata_path = config.state_dir() / "repository.json";
    const auto cache = cache_paths(config);
    const auto db_path = sqlite_store_path(config);

    if (!mkdir_p(config.data_dir(), error) || !mkdir_p(config.state_dir(), error)
        || !mkdir_p(cache.updates, error) || !mkdir_p(cache.incoming, error))
    {
        return false;
    }

    if (state.built && !path_exists(db_path))
    {
        HYD_LOG(warn, "sync state says the SQLite store is built, but the database is missing; rebuilding");
        state.built = false;
        state.next_index = 0;
        state.partial = false;
    }

    RepositoryClient repository(config.repository_url(), config.repository_access_key());
    repository.set_timeout_seconds(config.get_int("download_timeout_seconds", 120));
    repository.set_rate_limit_kib(static_cast<std::uint64_t>(
        std::max<std::int64_t>(0, config.get_int("download_rate_limit_kib", 0))));

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

    RepositoryMetadata fresh;
    if (!repository.fetch_metadata(state.next_index, fresh, error))
    {
        if (state.built)
        {
            HYD_LOGF(warn, "cannot fetch repository metadata, keeping the SQLite store: %s", error.c_str());
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

    std::vector<UpdateSource> sources;
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
            sources.push_back(std::move(source));
        }
    }

    state.last_sync_started = now_unix();
    if (!save_sync_state(state_path, state, error))
    {
        return false;
    }

    if (!download_updates(config, sources, callbacks, state, repository, error))
    {
        if (error == "stopped")
        {
            stopped_flag = true;
            error.clear();
            return true;
        }
        return false;
    }

    SqliteImporter importer;
    if (!importer.open(db_path, error))
    {
        return false;
    }

    UpdateParser parser;
    std::unordered_map<std::string, int> kinds;
    if (!load_update_kinds(config.state_dir() / "update_kinds.json", kinds, error))
    {
        return false;
    }

    std::map<std::int64_t, std::vector<const UpdateSource*>> by_index;
    for (const auto& source : sources)
    {
        by_index[source.update.index].push_back(&source);
    }

    std::size_t processed_indices = 0;

    for (const auto& [index, group] : by_index)
    {
        if (stopped(callbacks))
        {
            stopped_flag = true;
            error.clear();
            return true;
        }

        for (const auto* source : group)
        {
            auto bytes = read_file(source->cache_path, error);
            if (!bytes)
            {
                return false;
            }
            UpdateKind kind = UpdateKind::unknown;
            if (!detect_kind(parser, *bytes, kinds, source->update.hash, kind, error))
            {
                error = "update " + hex_encode(source->update.hash) + ": " + error;
                return false;
            }
        }

        if (!importer.begin(error))
        {
            return false;
        }

        try
        {
            SqliteDefinitionVisitor definition_visitor(importer);
            for (const auto* source : group)
            {
                const auto kind = kinds.find(hex_encode(source->update.hash));
                if (kind == kinds.end() || kind->second != static_cast<int>(UpdateKind::definitions))
                {
                    continue;
                }
                auto bytes = read_file(source->cache_path, error);
                if (!bytes)
                {
                    return false;
                }
                UpdateKind parsed_kind = UpdateKind::unknown;
                if (!parser.parse(*bytes, definition_visitor, parsed_kind, error))
                {
                    error = "update " + hex_encode(source->update.hash) + ": " + error;
                    return false;
                }
            }

            SqliteContentVisitor content_visitor(importer);
            for (const auto* source : group)
            {
                const auto kind = kinds.find(hex_encode(source->update.hash));
                if (kind == kinds.end() || kind->second != static_cast<int>(UpdateKind::content))
                {
                    continue;
                }
                auto bytes = read_file(source->cache_path, error);
                if (!bytes)
                {
                    return false;
                }
                UpdateKind parsed_kind = UpdateKind::unknown;
                if (!parser.parse(*bytes, content_visitor, parsed_kind, error))
                {
                    error = "update " + hex_encode(source->update.hash) + ": " + error;
                    return false;
                }
            }

            if (!importer.store_counts(error))
            {
                return false;
            }
        }
        catch (const std::exception& exception)
        {
            importer.rollback();
            error = std::string("SQLite import failed: ") + exception.what();
            return false;
        }

        if (!importer.commit(error))
        {
            return false;
        }

        state.next_index = index + 1;
        state.hash_count = importer.hash_count();
        state.mapping_count = importer.mapping_count();
        state.tag_count = importer.tag_count();
        if (!save_sync_state(state_path, state, error))
        {
            return false;
        }

        ++processed_indices;
        if (processed_indices % 25 == 0)
        {
            HYD_LOGF(info, "SQLite import: %zu indices, next index %lld, %llu mappings",
                processed_indices, static_cast<long long>(state.next_index),
                static_cast<unsigned long long>(state.mapping_count));
            std::string checkpoint_error;
            importer.checkpoint(checkpoint_error);
        }
    }

    if (!save_update_kinds(config.state_dir() / "update_kinds.json", kinds, error))
    {
        return false;
    }

    if (!state.built)
    {
        state.built = true;
        state.built_at = now_unix();
    }

    if (highest_target >= 0)
    {
        state.next_index = std::max(state.next_index, highest_target + 1);
    }

    state.partial = (max_index >= 0 && state.next_index > max_index);
    state.last_sync_at = now_unix();

    if (!save_sync_state(state_path, state, error))
    {
        return false;
    }

    if (callbacks.on_store_ready)
    {
        callbacks.on_store_ready();
    }

    if (!config.get_bool("keep_update_cache", false))
    {
        std::string cleanup_error;
        if (!clean_update_cache(config, cleanup_error))
        {
            HYD_LOGF(warn, "cannot clean update cache: %s", cleanup_error.c_str());
        }
    }

    HYD_LOGF(info, "SQLite sync complete: next index %lld, %llu hashes, %llu mappings, %llu tags",
        static_cast<long long>(state.next_index),
        static_cast<unsigned long long>(state.hash_count),
        static_cast<unsigned long long>(state.mapping_count),
        static_cast<unsigned long long>(state.tag_count));

    return true;
}

bool sqlite_vacuum(const Config& config, std::string& error)
{
    const auto path = sqlite_store_path(config);
    if (!path_exists(path))
    {
        error = "SQLite store does not exist: " + path.string();
        return false;
    }

    sqlite3* db = nullptr;
    if (sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READWRITE, nullptr) != SQLITE_OK)
    {
        error = "cannot open SQLite store " + path.string();
        if (db != nullptr)
        {
            sqlite3_close(db);
        }
        return false;
    }

    const bool checkpointed = sqlite_exec(db, "PRAGMA wal_checkpoint(TRUNCATE)", error);
    const bool vacuumed = checkpointed && sqlite_exec(db, "VACUUM", error);
    sqlite3_close(db);

    if (!vacuumed)
    {
        return false;
    }

    HYD_LOG(info, "SQLite store vacuumed");
    return true;
}

} // namespace hyd
