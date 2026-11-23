#include "hydrus/repository.hpp"

#include "common/log.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <set>

namespace hyd {

namespace {

constexpr int kSerialisableTypeMetadata = 37;
constexpr int kSerialisableTypeHydrusArgs = 21;
constexpr std::int64_t kHydrusArgsVersion = 2;
constexpr std::int64_t kMetadataVersion = 1;

// A metadata slice only lists update indices and their sha256 hashes. The
// current PTR slice is a few MiB; 64 MiB leaves room to grow while refusing a
// server that tries to make us buffer a huge JSON document.
constexpr std::size_t kMaxMetadataBytes = 64u * 1024u * 1024u;

// Legitimate update files are a few MiB compressed. Keep the per-file cap
// separate so a metadata request cannot use the larger update budget.
constexpr std::size_t kMaxUpdateFileBytes = 256u * 1024u * 1024u;

} // namespace

std::int64_t RepositoryMetadata::highest_index() const
{
    if (updates.empty())
    {
        return -1;
    }
    return updates.rbegin()->first;
}

std::size_t RepositoryMetadata::update_file_count() const
{
    std::size_t count = 0;
    for (const auto& [index, rows] : updates)
    {
        (void)index;
        count += rows.size();
    }
    return count;
}

std::vector<RepositoryUpdate> RepositoryMetadata::all_updates_ordered() const
{
    std::vector<RepositoryUpdate> result;
    result.reserve(update_file_count());
    for (const auto& [index, rows] : updates)
    {
        (void)index;
        result.insert(result.end(), rows.begin(), rows.end());
    }
    return result;
}

void RepositoryMetadata::merge(const RepositoryMetadata& other)
{
    for (const auto& [index, rows] : other.updates)
    {
        auto& target = updates[index];
        for (const auto& row : rows)
        {
            const bool duplicate = std::any_of(target.begin(), target.end(),
                [&row](const RepositoryUpdate& existing) { return existing.hash == row.hash; });
            if (!duplicate)
            {
                target.push_back(row);
            }
        }
    }

    if (other.next_update_due != 0)
    {
        next_update_due = other.next_update_due;
    }
}

RepositoryClient::RepositoryClient(std::string base_url, std::string access_key)
    : http_(std::move(base_url), std::move(access_key), "hydd/0.2")
{
}

bool RepositoryClient::fetch_metadata(std::int64_t since, RepositoryMetadata& metadata, std::string& error)
{
    http_.set_max_body_bytes(kMaxMetadataBytes);

    HttpResponse response;
    if (!http_.get("/metadata?since=" + std::to_string(since), response, error))
    {
        return false;
    }

    return parse_repository_metadata_bytes(response.body, metadata, error);
}

bool RepositoryClient::fetch_update(const Hash& hash, Bytes& compressed_body, std::string& error)
{
    http_.set_max_body_bytes(kMaxUpdateFileBytes);

    const std::string path = "/update?update_hash=" + hex_encode(hash);
    HttpResponse response;
    if (!http_.get(path, response, error))
    {
        return false;
    }

    const Hash actual = sha256(response.body);
    if (actual != hash)
    {
        error = "update sha256 mismatch: expected " + hex_encode(hash) + ", got " + hex_encode(actual);
        return false;
    }

    compressed_body = std::move(response.body);
    return true;
}

bool parse_repository_metadata_bytes(std::span<const std::uint8_t> compressed, RepositoryMetadata& metadata, std::string& error)
{
    Bytes text;
    if (!zlib_inflate(compressed, text, error))
    {
        error = "cannot inflate repository metadata: " + error;
        return false;
    }

    return parse_repository_metadata_json(
        std::string_view(reinterpret_cast<const char*>(text.data()), text.size()), metadata, error);
}

bool parse_repository_metadata_json(std::string_view json_text, RepositoryMetadata& metadata, std::string& error)
{
    try
    {
        const auto root = nlohmann::json::parse(json_text);
        if (!root.is_array() || root.size() < 3)
        {
            error = "repository metadata is not a serialisable tuple";
            return false;
        }

        const auto type = root.at(0).get<int>();
        if (type != kSerialisableTypeHydrusArgs)
        {
            error = "repository metadata has unexpected serialisable type " + std::to_string(type);
            return false;
        }

        const auto outer_version = root.at(1).get<std::int64_t>();
        if (outer_version != kHydrusArgsVersion)
        {
            error = "repository metadata has unsupported HydrusArgs version "
                + std::to_string(outer_version) + " (this build understands "
                + std::to_string(kHydrusArgsVersion) + ")";
            return false;
        }

        const auto& args = root.at(2);
        if (!args.is_array() || args.empty())
        {
            error = "repository metadata has no arguments";
            return false;
        }

        const nlohmann::json* value_pointer = nullptr;
        for (const auto& argument : args)
        {
            if (!argument.is_array() || argument.size() < 2)
            {
                continue;
            }

            const auto& key_meta = argument.at(0);
            if (!key_meta.is_array() || key_meta.size() < 2 || !key_meta.at(1).is_string()
                || key_meta.at(1).get<std::string>() != "metadata_slice")
            {
                continue;
            }

            const auto& value_meta = argument.at(1);
            if (value_meta.is_array() && value_meta.size() >= 2)
            {
                value_pointer = &value_meta.at(1);
            }
        }

        if (value_pointer == nullptr)
        {
            error = "repository metadata response does not contain a metadata_slice";
            return false;
        }

        const auto& value = *value_pointer;
        if (!value.is_array() || value.size() < 3 || value.at(0).get<int>() != kSerialisableTypeMetadata)
        {
            error = "metadata_slice has an unexpected type";
            return false;
        }

        const auto metadata_version = value.at(1).get<std::int64_t>();
        if (metadata_version != kMetadataVersion)
        {
            error = "metadata_slice has unsupported version " + std::to_string(metadata_version)
                + " (this build understands " + std::to_string(kMetadataVersion) + ")";
            return false;
        }

        const auto& info = value.at(2);
        if (!info.is_array() || info.size() < 2)
        {
            error = "metadata_slice is malformed";
            return false;
        }

        RepositoryMetadata parsed;
        parsed.next_update_due = info.at(1).get<std::int64_t>();

        for (const auto& row : info.at(0))
        {
            if (!row.is_array() || row.size() < 4)
            {
                error = "metadata_slice row is malformed";
                return false;
            }

            RepositoryUpdate update;
            update.index = row.at(0).get<std::int64_t>();
            update.begin = row.at(2).get<std::int64_t>();
            update.end = row.at(3).get<std::int64_t>();

            // Indices are a non-negative sequence and begin/end are unix
            // timestamps. The official server only ever emits sane values;
            // reject nonsense instead of carrying it into the stored metadata.
            if (update.index < 0)
            {
                error = "metadata_slice contains a negative update index";
                return false;
            }

            if (update.begin < 0 || update.end < update.begin)
            {
                error = "metadata_slice contains an invalid update time range";
                return false;
            }

            for (const auto& encoded_hash : row.at(1))
            {
                const auto hash = hash_from_hex(encoded_hash.get<std::string>());
                if (!hash)
                {
                    error = "metadata_slice contains an invalid sha256";
                    return false;
                }
                RepositoryUpdate copy = update;
                copy.hash = *hash;
                parsed.updates[copy.index].push_back(copy);
            }

            if (row.at(1).empty())
            {
                // Preserve empty indices so progress reporting knows they
                // exist, without inventing a hash.
                parsed.updates[update.index];
            }
        }

        metadata.merge(parsed);
        return true;
    }
    catch (const std::exception& exception)
    {
        error = std::string("cannot parse repository metadata: ") + exception.what();
        return false;
    }
}

bool load_repository_metadata_file(const std::filesystem::path& path, RepositoryMetadata& metadata, std::string& error)
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
        metadata.next_update_due = root.value("next_update_due", static_cast<std::int64_t>(0));

        for (const auto& row : root.at("updates"))
        {
            RepositoryUpdate update;
            update.index = row.at(0).get<std::int64_t>();
            update.begin = row.at(1).get<std::int64_t>();
            update.end = row.at(2).get<std::int64_t>();

            if (update.index < 0)
            {
                error = "stored repository metadata contains a negative update index";
                return false;
            }

            if (update.begin < 0 || update.end < update.begin)
            {
                error = "stored repository metadata contains an invalid update time range";
                return false;
            }

            for (const auto& encoded_hash : row.at(3))
            {
                const auto hash = hash_from_hex(encoded_hash.get<std::string>());
                if (!hash)
                {
                    error = "stored repository metadata contains an invalid sha256";
                    return false;
                }
                update.hash = *hash;
                metadata.updates[update.index].push_back(update);
            }

            if (row.at(3).empty())
            {
                metadata.updates[update.index];
            }
        }

        return true;
    }
    catch (const std::exception& exception)
    {
        error = std::string("cannot parse stored repository metadata: ") + exception.what();
        return false;
    }
}

bool save_repository_metadata_file(const std::filesystem::path& path, const RepositoryMetadata& metadata, std::string& error)
{
    nlohmann::json root;
    root["next_update_due"] = metadata.next_update_due;
    root["updates"] = nlohmann::json::array();

    for (const auto& [index, rows] : metadata.updates)
    {
        nlohmann::json row = nlohmann::json::array();
        row.push_back(index);

        std::int64_t begin = 0;
        std::int64_t end = 0;
        if (!rows.empty())
        {
            begin = rows.front().begin;
            end = rows.front().end;
        }

        row.push_back(begin);
        row.push_back(end);

        nlohmann::json hashes = nlohmann::json::array();
        for (const auto& update : rows)
        {
            hashes.push_back(hex_encode(update.hash));
        }
        row.push_back(std::move(hashes));

        root["updates"].push_back(std::move(row));
    }

    return write_file_atomic(path, root.dump(), error);
}

} // namespace hyd
