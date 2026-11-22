#include "store/delta.hpp"

#include "common/log.hpp"
#include "store/format.hpp"

#include <zlib.h>

#include <algorithm>
#include <fstream>
#include <map>

namespace hyd {

void DeltaData::init(std::size_t partitions)
{
    hashes_by_partition.assign(partitions, {});
    legacy_hashes.clear();
}

void DeltaData::absorb(const DeltaData& other)
{
    for (const auto& [id, tag] : other.tags)
    {
        tags[id] = tag;
    }

    if (hashes_by_partition.size() < other.hashes_by_partition.size())
    {
        hashes_by_partition.resize(other.hashes_by_partition.size());
    }
    for (std::size_t i = 0; i < other.hashes_by_partition.size(); ++i)
    {
        auto& target = hashes_by_partition[i];
        target.insert(target.end(), other.hashes_by_partition[i].begin(), other.hashes_by_partition[i].end());
    }
    legacy_hashes.insert(legacy_hashes.end(), other.legacy_hashes.begin(), other.legacy_hashes.end());

    for (const auto& [hash_id, tag_ids] : other.mapping_additions)
    {
        auto& target = mapping_additions[hash_id];
        for (const auto tag_id : tag_ids)
        {
            mapping_deletions.erase(pair_key(hash_id, tag_id));
            if (std::find(target.begin(), target.end(), tag_id) == target.end())
            {
                target.push_back(tag_id);
            }
        }
    }

    for (const auto key : other.mapping_deletions)
    {
        const auto hash_id = static_cast<std::uint32_t>(key >> 32);
        const auto tag_id = static_cast<std::uint32_t>(key & 0xffffffffu);
        auto iterator = mapping_additions.find(hash_id);
        if (iterator != mapping_additions.end())
        {
            auto& list = iterator->second;
            list.erase(std::remove(list.begin(), list.end(), tag_id), list.end());
            if (list.empty())
            {
                mapping_additions.erase(iterator);
            }
        }
        mapping_deletions.insert(key);
    }
    deleted_files.insert(other.deleted_files.begin(), other.deleted_files.end());

    for (const auto& [bad, good] : other.sibling_additions)
    {
        sibling_deletions.erase(pair_key(bad, good));
        sibling_additions[bad] = good;
    }
    for (const auto key : other.sibling_deletions)
    {
        const auto bad = static_cast<std::uint32_t>(key >> 32);
        const auto good = static_cast<std::uint32_t>(key & 0xffffffffu);
        const auto iterator = sibling_additions.find(bad);
        if (iterator != sibling_additions.end() && iterator->second == good)
        {
            sibling_additions.erase(iterator);
        }
        sibling_deletions.insert(key);
    }

    for (const auto& [child, parents] : other.parent_additions)
    {
        auto& target = parent_additions[child];
        for (const auto parent : parents)
        {
            parent_deletions.erase(pair_key(child, parent));
            if (std::find(target.begin(), target.end(), parent) == target.end())
            {
                target.push_back(parent);
            }
        }
    }
    for (const auto key : other.parent_deletions)
    {
        const auto child = static_cast<std::uint32_t>(key >> 32);
        const auto parent = static_cast<std::uint32_t>(key & 0xffffffffu);
        auto iterator = parent_additions.find(child);
        if (iterator != parent_additions.end())
        {
            auto& list = iterator->second;
            list.erase(std::remove(list.begin(), list.end(), parent), list.end());
            if (list.empty())
            {
                parent_additions.erase(iterator);
            }
        }
        parent_deletions.insert(key);
    }

    record_count += other.record_count;
    mapping_add_count += other.mapping_add_count;
    mapping_delete_count += other.mapping_delete_count;
}

std::int64_t DeltaData::approximate_bytes() const
{
    std::int64_t total = 0;
    for (const auto& [id, tag] : tags)
    {
        (void)id;
        total += static_cast<std::int64_t>(tag.size()) + 16;
    }
    for (const auto& partition : hashes_by_partition)
    {
        total += static_cast<std::int64_t>(partition.size()) * 40;
    }
    total += static_cast<std::int64_t>(legacy_hashes.size()) * 40;
    for (const auto& [hash_id, tag_ids] : mapping_additions)
    {
        (void)hash_id;
        total += static_cast<std::int64_t>(tag_ids.size()) * 8 + 16;
    }
    total += static_cast<std::int64_t>(mapping_deletions.size()) * 16;
    total += static_cast<std::int64_t>(deleted_files.size()) * 8;
    total += static_cast<std::int64_t>(sibling_additions.size()) * 12;
    total += static_cast<std::int64_t>(sibling_deletions.size()) * 16;
    for (const auto& [child, parents] : parent_additions)
    {
        (void)child;
        total += static_cast<std::int64_t>(parents.size()) * 8 + 16;
    }
    total += static_cast<std::int64_t>(parent_deletions.size()) * 16;
    return total;
}

namespace {

} // namespace

bool load_deltas(const std::filesystem::path& directory, std::size_t partitions, DeltaData& data,
    bool& truncated, std::string& error)
{
    truncated = false;
    data.init(partitions);

    if (!path_exists(directory))
    {
        return true;
    }

    std::map<std::string, std::filesystem::path> files;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(directory, ec))
    {
        if (!entry.is_regular_file())
        {
            continue;
        }
        const auto name = entry.path().filename().string();
        if (starts_with(name, "d-") && ends_with(name, ".bin"))
        {
            files[name] = entry.path();
        }
    }

    if (ec)
    {
        error = "cannot scan delta directory: " + ec.message();
        return false;
    }

    for (const auto& [name, path] : files)
    {
        auto bytes = read_file(path, error);
        if (!bytes)
        {
            return false;
        }

        std::size_t offset = 0;
        const std::size_t size = bytes->size();

        while (offset < size)
        {
            if (size - offset < 9)
            {
                truncated = true;
                break;
            }

            const std::uint8_t type = (*bytes)[offset];
            const std::uint32_t length = read_u32(bytes->data() + offset + 1);
            if (length > size - offset - 9)
            {
                truncated = true;
                break;
            }

            const std::uint8_t* payload = bytes->data() + offset + 5;
            const std::uint32_t crc = read_u32(bytes->data() + offset + 5 + length);
            const std::uint32_t actual_crc = static_cast<std::uint32_t>(
                crc32(0, payload, length));

            if (crc != actual_crc)
            {
                truncated = true;
                break;
            }

            offset += 5 + length + 4;

            switch (type)
            {
                case kDeltaTagDefinition:
                {
                    if (length < 6)
                    {
                        error = "delta tag definition is too short";
                        return false;
                    }
                    const std::uint32_t id = read_u32(payload);
                    const std::uint16_t text_length = static_cast<std::uint16_t>(
                        payload[4] | (static_cast<std::uint16_t>(payload[5]) << 8));
                    if (length < 6u + static_cast<std::uint32_t>(text_length))
                    {
                        error = "delta tag definition is truncated";
                        return false;
                    }
                    data.tags[id] = std::string(
                        reinterpret_cast<const char*>(payload + 6), text_length);
                    break;
                }
                case kDeltaHashDefinition:
                {
                    if (length != 36)
                    {
                        error = "delta hash definition has unexpected size";
                        return false;
                    }
                    HashEntry entry;
                    entry.hash_id = read_u32(payload);
                    std::copy(payload + 4, payload + 36, entry.hash.begin());
                    if (data.hashes_by_partition.empty())
                    {
                        data.init(partitions);
                    }
                    const auto partition = static_cast<std::size_t>(
                        entry.hash[0] * partitions / 256);
                    data.hashes_by_partition[partition].push_back(entry);
                    break;
                }
                case kDeltaLegacyHashDefinition:
                {
                    if (length != 37)
                    {
                        error = "delta legacy hash definition has unexpected size";
                        return false;
                    }
                    LegacyHashEntry entry;
                    entry.hash_id = read_u32(payload);
                    entry.length = payload[4];
                    if (entry.length != 16 && entry.length != 20 && entry.length != 32)
                    {
                        error = "delta legacy hash definition has an unsupported length";
                        return false;
                    }
                    std::copy(payload + 5, payload + 37, entry.hash.begin());
                    data.legacy_hashes.push_back(entry);
                    break;
                }
                case kDeltaMappingAdd:
                {
                    if (length != 8)
                    {
                        error = "delta mapping-add record has unexpected size";
                        return false;
                    }
                    const std::uint32_t hash_id = read_u32(payload);
                    const std::uint32_t tag_id = read_u32(payload + 4);
                    data.mapping_deletions.erase(pair_key(hash_id, tag_id));
                    auto& list = data.mapping_additions[hash_id];
                    if (std::find(list.begin(), list.end(), tag_id) == list.end())
                    {
                        list.push_back(tag_id);
                    }
                    ++data.mapping_add_count;
                    break;
                }
                case kDeltaMappingDelete:
                {
                    if (length != 8)
                    {
                        error = "delta mapping-delete record has unexpected size";
                        return false;
                    }
                    const std::uint32_t hash_id = read_u32(payload);
                    const std::uint32_t tag_id = read_u32(payload + 4);
                    const auto key = pair_key(hash_id, tag_id);
                    auto iterator = data.mapping_additions.find(hash_id);
                    if (iterator != data.mapping_additions.end())
                    {
                        auto& list = iterator->second;
                        list.erase(std::remove(list.begin(), list.end(), tag_id), list.end());
                        if (list.empty())
                        {
                            data.mapping_additions.erase(iterator);
                        }
                    }
                    data.mapping_deletions.insert(key);
                    ++data.mapping_delete_count;
                    break;
                }
                case kDeltaFileDelete:
                {
                    if (length != 4)
                    {
                        error = "delta file-delete record has unexpected size";
                        return false;
                    }
                    data.deleted_files.insert(read_u32(payload));
                    break;
                }
                case kDeltaSiblingAdd:
                case kDeltaSiblingDelete:
                case kDeltaParentAdd:
                case kDeltaParentDelete:
                {
                    if (length != 8)
                    {
                        error = "delta tag pair record has unexpected size";
                        return false;
                    }
                    const std::uint32_t first = read_u32(payload);
                    const std::uint32_t second = read_u32(payload + 4);

                    if (type == kDeltaSiblingAdd)
                    {
                        data.sibling_deletions.erase(pair_key(first, second));
                        data.sibling_additions[first] = second;
                    }
                    else if (type == kDeltaSiblingDelete)
                    {
                        const auto iterator = data.sibling_additions.find(first);
                        if (iterator != data.sibling_additions.end() && iterator->second == second)
                        {
                            data.sibling_additions.erase(iterator);
                        }
                        data.sibling_deletions.insert(pair_key(first, second));
                    }
                    else if (type == kDeltaParentAdd)
                    {
                        data.parent_deletions.erase(pair_key(first, second));
                        auto& list = data.parent_additions[first];
                        if (std::find(list.begin(), list.end(), second) == list.end())
                        {
                            list.push_back(second);
                        }
                    }
                    else
                    {
                        auto iterator = data.parent_additions.find(first);
                        if (iterator != data.parent_additions.end())
                        {
                            auto& list = iterator->second;
                            list.erase(std::remove(list.begin(), list.end(), second), list.end());
                            if (list.empty())
                            {
                                data.parent_additions.erase(iterator);
                            }
                        }
                        data.parent_deletions.insert(pair_key(first, second));
                    }
                    break;
                }
                default:
                {
                    error = "delta contains unknown record type " + std::to_string(type);
                    return false;
                }
            }

            ++data.record_count;
        }

        if (truncated)
        {
            HYD_LOGF(warn, "delta file %s has a truncated tail; ignoring the remainder", path.string().c_str());
            break;
        }
    }

    for (auto& partition : data.hashes_by_partition)
    {
        std::sort(partition.begin(), partition.end(), hash_entry_less);
    }

    std::sort(data.legacy_hashes.begin(), data.legacy_hashes.end(), legacy_hash_entry_less);

    return true;
}

} // namespace hyd
