#include "store/sqlite_store.hpp"

#include "common/log.hpp"

#include <sqlite3.h>

#include <algorithm>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

namespace hyd {

struct SqliteStore::Impl
{
    sqlite3* db = nullptr;
    mutable std::mutex mutex;

    sqlite3_stmt* find_hash = nullptr;
    sqlite3_stmt* tags_for_hash = nullptr;
    sqlite3_stmt* tag_by_id = nullptr;

    std::unordered_map<std::uint32_t, std::uint32_t> siblings;
    std::unordered_map<std::uint32_t, std::vector<std::uint32_t>> parents;

    std::uint64_t hash_count = 0;
    std::uint64_t mapping_count = 0;
    std::uint64_t tag_count = 0;
};

SqliteStore::SqliteStore()
    : impl_(new Impl())
{
}

SqliteStore::~SqliteStore()
{
    if (impl_->db != nullptr)
    {
        if (impl_->find_hash != nullptr) sqlite3_finalize(impl_->find_hash);
        if (impl_->tags_for_hash != nullptr) sqlite3_finalize(impl_->tags_for_hash);
        if (impl_->tag_by_id != nullptr) sqlite3_finalize(impl_->tag_by_id);
        sqlite3_close(impl_->db);
    }
    delete impl_;
}

bool SqliteStore::open(const std::filesystem::path& path, std::string& error)
{
    open_ = false;

    const int flags = SQLITE_OPEN_READONLY | SQLITE_OPEN_NOMUTEX;
    if (sqlite3_open_v2(path.c_str(), &impl_->db, flags, nullptr) != SQLITE_OK)
    {
        error = "cannot open SQLite store " + path.string() + ": "
            + (impl_->db != nullptr ? sqlite3_errmsg(impl_->db) : "unknown error");
        return false;
    }

    sqlite3_busy_timeout(impl_->db, 5000);

    {
        sqlite3_stmt* statement = nullptr;
        if (sqlite3_prepare_v2(impl_->db,
                "SELECT name FROM sqlite_master WHERE type='table' AND name='hashes'", -1,
                &statement, nullptr) != SQLITE_OK)
        {
            error = std::string("cannot inspect SQLite store: ") + sqlite3_errmsg(impl_->db);
            return false;
        }
        const bool has_table = sqlite3_step(statement) == SQLITE_ROW;
        sqlite3_finalize(statement);
        if (!has_table)
        {
            error = path.string() + " is not an initialized hydd SQLite store";
            return false;
        }
    }

    const char* queries[] = {
        "SELECT hash_id FROM hashes WHERE hash = ?1",
        "SELECT m.tag_id, t.tag FROM mappings m JOIN tags t ON t.tag_id = m.tag_id WHERE m.hash_id = ?1",
        "SELECT tag FROM tags WHERE tag_id = ?1",
    };
    sqlite3_stmt** statements[] = { &impl_->find_hash, &impl_->tags_for_hash, &impl_->tag_by_id };

    for (std::size_t i = 0; i < 3; ++i)
    {
        if (sqlite3_prepare_v2(impl_->db, queries[i], -1, statements[i], nullptr) != SQLITE_OK)
        {
            error = std::string("cannot prepare SQLite query: ") + sqlite3_errmsg(impl_->db);
            return false;
        }
    }

    auto read_pairs = [this](const char* query,
                             std::unordered_map<std::uint32_t, std::vector<std::uint32_t>>& target) {
        sqlite3_stmt* statement = nullptr;
        if (sqlite3_prepare_v2(impl_->db, query, -1, &statement, nullptr) != SQLITE_OK)
        {
            return false;
        }
        while (sqlite3_step(statement) == SQLITE_ROW)
        {
            const auto first = static_cast<std::uint32_t>(sqlite3_column_int64(statement, 0));
            const auto second = static_cast<std::uint32_t>(sqlite3_column_int64(statement, 1));
            auto& list = target[first];
            if (std::find(list.begin(), list.end(), second) == list.end())
            {
                list.push_back(second);
            }
        }
        sqlite3_finalize(statement);
        return true;
    };

    {
        std::unordered_map<std::uint32_t, std::vector<std::uint32_t>> raw_siblings;
        if (!read_pairs("SELECT bad_tag_id, good_tag_id FROM siblings", raw_siblings))
        {
            error = std::string("cannot read sibling tags: ") + sqlite3_errmsg(impl_->db);
            return false;
        }
        for (const auto& [bad, goods] : raw_siblings)
        {
            if (!goods.empty())
            {
                impl_->siblings[bad] = goods.front();
            }
        }
    }

    if (!read_pairs("SELECT child_tag_id, parent_tag_id FROM parents", impl_->parents))
    {
        error = std::string("cannot read parent tags: ") + sqlite3_errmsg(impl_->db);
        return false;
    }

    {
        sqlite3_stmt* statement = nullptr;
        if (sqlite3_prepare_v2(impl_->db, "SELECT key, value FROM meta", -1, &statement, nullptr) == SQLITE_OK)
        {
            while (sqlite3_step(statement) == SQLITE_ROW)
            {
                const std::string key = reinterpret_cast<const char*>(sqlite3_column_text(statement, 0));
                const auto value = static_cast<std::uint64_t>(sqlite3_column_int64(statement, 1));
                if (key == "hash_count") impl_->hash_count = value;
                else if (key == "mapping_count") impl_->mapping_count = value;
                else if (key == "tag_count") impl_->tag_count = value;
            }
            sqlite3_finalize(statement);
        }
    }

    open_ = true;
    return true;
}

TagQueryResult SqliteStore::lookup(std::span<const std::uint8_t> hash) const
{
    TagQueryResult result;
    if (!open_)
    {
        return result;
    }

    std::lock_guard<std::mutex> lock(impl_->mutex);

    sqlite3_reset(impl_->find_hash);
    sqlite3_bind_blob(impl_->find_hash, 1, hash.data(), static_cast<int>(hash.size()), SQLITE_TRANSIENT);

    if (sqlite3_step(impl_->find_hash) != SQLITE_ROW)
    {
        return result;
    }

    const auto hash_id = static_cast<std::uint32_t>(sqlite3_column_int64(impl_->find_hash, 0));
    result.found = true;
    result.hash_id = hash_id;

    std::vector<std::uint32_t> tag_ids;
    std::unordered_map<std::uint32_t, std::string> known_tags;

    sqlite3_reset(impl_->tags_for_hash);
    sqlite3_bind_int64(impl_->tags_for_hash, 1, static_cast<sqlite3_int64>(hash_id));

    while (sqlite3_step(impl_->tags_for_hash) == SQLITE_ROW)
    {
        const auto tag_id = static_cast<std::uint32_t>(sqlite3_column_int64(impl_->tags_for_hash, 0));
        const auto* text = sqlite3_column_text(impl_->tags_for_hash, 1);
        const int length = sqlite3_column_bytes(impl_->tags_for_hash, 1);
        std::string tag(reinterpret_cast<const char*>(text), static_cast<std::size_t>(length));

        if (std::find(tag_ids.begin(), tag_ids.end(), tag_id) == tag_ids.end())
        {
            tag_ids.push_back(tag_id);
        }
        known_tags[tag_id] = std::move(tag);
    }

    auto resolve_sibling = [this](std::uint32_t tag_id) {
        std::unordered_set<std::uint32_t> visited;
        while (true)
        {
            if (!visited.insert(tag_id).second)
            {
                return tag_id;
            }
            const auto iterator = impl_->siblings.find(tag_id);
            if (iterator == impl_->siblings.end() || iterator->second == tag_id)
            {
                return tag_id;
            }
            tag_id = iterator->second;
        }
    };

    std::vector<std::uint32_t> display_ids;
    display_ids.reserve(tag_ids.size());
    for (const auto tag_id : tag_ids)
    {
        display_ids.push_back(resolve_sibling(tag_id));
    }

    std::sort(display_ids.begin(), display_ids.end());
    display_ids.erase(std::unique(display_ids.begin(), display_ids.end()), display_ids.end());

    std::vector<std::uint32_t> queue = display_ids;
    std::unordered_set<std::uint32_t> seen(display_ids.begin(), display_ids.end());
    for (std::size_t i = 0; i < queue.size(); ++i)
    {
        const auto iterator = impl_->parents.find(queue[i]);
        if (iterator == impl_->parents.end())
        {
            continue;
        }
        for (const auto parent : iterator->second)
        {
            const auto ideal = resolve_sibling(parent);
            if (seen.insert(ideal).second)
            {
                display_ids.push_back(ideal);
                queue.push_back(ideal);
            }
        }
    }

    std::sort(display_ids.begin(), display_ids.end());
    display_ids.erase(std::unique(display_ids.begin(), display_ids.end()), display_ids.end());

    for (const auto tag_id : tag_ids)
    {
        result.storage_tags.push_back(known_tags[tag_id]);
    }

    for (const auto tag_id : display_ids)
    {
        const auto known = known_tags.find(tag_id);
        if (known != known_tags.end())
        {
            result.display_tags.push_back(known->second);
            continue;
        }

        sqlite3_reset(impl_->tag_by_id);
        sqlite3_bind_int64(impl_->tag_by_id, 1, static_cast<sqlite3_int64>(tag_id));
        if (sqlite3_step(impl_->tag_by_id) == SQLITE_ROW)
        {
            const auto* text = sqlite3_column_text(impl_->tag_by_id, 0);
            const int length = sqlite3_column_bytes(impl_->tag_by_id, 0);
            result.display_tags.emplace_back(reinterpret_cast<const char*>(text), static_cast<std::size_t>(length));
        }
    }

    std::sort(result.storage_tags.begin(), result.storage_tags.end());
    result.storage_tags.erase(std::unique(result.storage_tags.begin(), result.storage_tags.end()),
        result.storage_tags.end());
    std::sort(result.display_tags.begin(), result.display_tags.end());
    result.display_tags.erase(std::unique(result.display_tags.begin(), result.display_tags.end()),
        result.display_tags.end());

    return result;
}

std::uint64_t SqliteStore::total_hash_count() const { return impl_->hash_count; }
std::uint64_t SqliteStore::total_mapping_count() const { return impl_->mapping_count; }
std::uint64_t SqliteStore::total_tag_count() const { return impl_->tag_count; }

} // namespace hyd
