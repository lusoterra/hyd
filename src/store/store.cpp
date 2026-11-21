#include "store/store.hpp"

#include "common/log.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <unordered_set>

namespace hyd {

namespace {

class MappedFile
{
public:
    MappedFile() = default;

    ~MappedFile()
    {
        if (data_ != nullptr)
        {
            ::munmap(data_, size_);
        }
    }

    MappedFile(const MappedFile&) = delete;
    MappedFile& operator=(const MappedFile&) = delete;

    MappedFile(MappedFile&& other) noexcept
        : data_(other.data_), size_(other.size_)
    {
        other.data_ = nullptr;
        other.size_ = 0;
    }

    MappedFile& operator=(MappedFile&& other) noexcept
    {
        if (this != &other)
        {
            if (data_ != nullptr)
            {
                ::munmap(data_, size_);
            }
            data_ = other.data_;
            size_ = other.size_;
            other.data_ = nullptr;
            other.size_ = 0;
        }
        return *this;
    }

    bool open(const std::filesystem::path& path, std::string& error)
    {
        const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0)
        {
            error = "cannot open " + path.string() + ": " + errno_string(errno);
            return false;
        }

        struct stat info {};
        if (::fstat(fd, &info) != 0)
        {
            error = "cannot stat " + path.string() + ": " + errno_string(errno);
            ::close(fd);
            return false;
        }

        size_ = static_cast<std::size_t>(info.st_size);
        if (size_ > 0)
        {
            data_ = ::mmap(nullptr, size_, PROT_READ, MAP_SHARED, fd, 0);
            if (data_ == MAP_FAILED)
            {
                data_ = nullptr;
                error = "cannot mmap " + path.string() + ": " + errno_string(errno);
                ::close(fd);
                return false;
            }
            ::madvise(data_, size_, MADV_RANDOM);
        }

        ::close(fd);
        return true;
    }

    const std::uint8_t* data() const { return static_cast<const std::uint8_t*>(data_); }
    std::size_t size() const { return size_; }

private:
    void* data_ = nullptr;
    std::size_t size_ = 0;
};

HashEntry entry_at(const std::uint8_t* base, std::size_t index)
{
    HashEntry entry;
    std::memcpy(entry.hash.data(), base + index * sizeof(HashEntry), 32);
    entry.hash_id = read_u32(base + index * sizeof(HashEntry) + 32);
    return entry;
}

struct PartitionFiles
{
    MappedFile hashes;
    MappedFile maps;

    const std::uint8_t* hash_entries = nullptr;
    std::uint32_t hash_count = 0;

    const std::uint8_t* map_hash_ids = nullptr;
    const std::uint8_t* map_offsets = nullptr;
    const std::uint8_t* map_tags = nullptr;
    std::uint32_t map_count = 0;
    std::uint32_t map_tag_count = 0;
};

struct TagSegment
{
    std::uint64_t base = 0;
    std::uint64_t last = 0;
    std::uint64_t count = 0;
    MappedFile idx;
    MappedFile str;
    MappedFile ids;
};

} // namespace

struct Store::Impl
{
    std::vector<PartitionFiles> partitions;
    std::vector<TagSegment> tag_segments;

    std::unordered_map<std::uint32_t, std::uint32_t> siblings;
    std::unordered_map<std::uint32_t, std::vector<std::uint32_t>> parents;
};

Store::Store()
    : impl_(new Impl())
{
}

Store::~Store()
{
    delete impl_;
}

std::uint32_t Store::partition_for(const Hash& hash) const
{
    const auto parts = static_cast<std::uint32_t>(std::max<std::size_t>(1, manifest_.partitions));
    return (static_cast<std::uint32_t>(hash[0]) * parts) / 256;
}

bool Store::open(const std::filesystem::path& store_dir, std::string& error)
{
    open_ = false;
    manifest_ = StoreManifest {};
    delta_ = DeltaData {};

    if (!load_store_manifest(store_dir, manifest_, error))
    {
        return false;
    }

    if (manifest_.format != kStoreFormatVersion)
    {
        error = "store format " + std::to_string(manifest_.format) + " is not supported";
        return false;
    }

    impl_->partitions.clear();
    impl_->partitions.resize(manifest_.partitions);

    for (std::uint32_t p = 0; p < manifest_.partitions; ++p)
    {
        char name[16];
        std::snprintf(name, sizeof(name), "p%02u", p);

        const auto partition_dir = store_dir / name;
        auto& files = impl_->partitions[p];

        if (!files.hashes.open(partition_dir / "hashes.bin", error))
        {
            return false;
        }
        if (!files.maps.open(partition_dir / "maps.bin", error))
        {
            return false;
        }

        if (files.hashes.size() < 12 + 0)
        {
            error = "hashes.bin is truncated in " + partition_dir.string();
            return false;
        }
        const auto* hashes_data = files.hashes.data();
        if (std::memcmp(hashes_data, kHashesMagic, 8) != 0)
        {
            error = "bad hashes.bin magic in " + partition_dir.string();
            return false;
        }
        files.hash_count = read_u32(hashes_data + 8);
        if (files.hashes.size() < 12 + static_cast<std::size_t>(files.hash_count) * sizeof(HashEntry))
        {
            error = "hashes.bin has an invalid count in " + partition_dir.string();
            return false;
        }
        files.hash_entries = hashes_data + 12;

        if (files.maps.size() < 20)
        {
            error = "maps.bin is truncated in " + partition_dir.string();
            return false;
        }
        const auto* maps_data = files.maps.data();
        if (std::memcmp(maps_data, kMapsMagic, 8) != 0)
        {
            error = "bad maps.bin magic in " + partition_dir.string();
            return false;
        }
        files.map_count = read_u32(maps_data + 8);
        files.map_tag_count = read_u32(maps_data + 12);

        const std::size_t expected = 20
            + static_cast<std::size_t>(files.map_count) * 4
            + (static_cast<std::size_t>(files.map_count) + 1) * 8
            + static_cast<std::size_t>(files.map_tag_count) * 4;
        if (files.maps.size() < expected)
        {
            error = "maps.bin has an invalid count in " + partition_dir.string();
            return false;
        }

        files.map_hash_ids = maps_data + 20;
        files.map_offsets = maps_data + 20 + static_cast<std::size_t>(files.map_count) * 4;
        files.map_tags = files.map_offsets + (static_cast<std::size_t>(files.map_count) + 1) * 8;
    }

    impl_->tag_segments.clear();
    impl_->tag_segments.reserve(manifest_.tag_segments.size());
    for (const auto& info : manifest_.tag_segments)
    {
        TagSegment segment;
        segment.base = info.base;
        segment.last = info.last;
        segment.count = info.count;
        if (!segment.idx.open(store_dir / info.idx_path, error))
        {
            return false;
        }
        if (!segment.str.open(store_dir / info.str_path, error))
        {
            return false;
        }
        if (!segment.ids.open(store_dir / info.ids_path, error))
        {
            return false;
        }
        if (segment.idx.size() < info.count * 12)
        {
            error = "tag segment index is truncated: " + info.idx_path;
            return false;
        }
        if (segment.ids.size() < info.count * 4)
        {
            error = "tag segment id list is truncated: " + info.ids_path;
            return false;
        }
        impl_->tag_segments.push_back(std::move(segment));
    }

    impl_->siblings.clear();
    impl_->parents.clear();

    {
        const auto path = store_dir / "siblings.bin";
        if (path_exists(path))
        {
            auto bytes = read_file(path, error);
            if (!bytes)
            {
                return false;
            }
            if (bytes->size() >= 12 && std::memcmp(bytes->data(), kPairsMagic, 8) == 0)
            {
                const auto count = read_u32(bytes->data() + 8);
                if (bytes->size() >= 12 + static_cast<std::size_t>(count) * 8)
                {
                    for (std::uint32_t i = 0; i < count; ++i)
                    {
                        const auto* row = bytes->data() + 12 + i * 8;
                        impl_->siblings[read_u32(row)] = read_u32(row + 4);
                    }
                }
            }
        }
    }

    {
        const auto path = store_dir / "parents.bin";
        if (path_exists(path))
        {
            auto bytes = read_file(path, error);
            if (!bytes)
            {
                return false;
            }
            if (bytes->size() >= 12 && std::memcmp(bytes->data(), kPairsMagic, 8) == 0)
            {
                const auto count = read_u32(bytes->data() + 8);
                if (bytes->size() >= 12 + static_cast<std::size_t>(count) * 8)
                {
                    for (std::uint32_t i = 0; i < count; ++i)
                    {
                        const auto* row = bytes->data() + 12 + i * 8;
                        impl_->parents[read_u32(row)].push_back(read_u32(row + 4));
                    }
                }
            }
        }
    }

    bool truncated = false;
    if (!load_deltas(store_dir / "delta", manifest_.partitions, delta_, truncated, error))
    {
        return false;
    }
    if (truncated)
    {
        HYD_LOG(warn, "the delta store has a truncated tail (a previous sync was interrupted); ignoring it");
    }

    for (const auto& [bad, good] : delta_.sibling_additions)
    {
        impl_->siblings[bad] = good;
    }
    for (const auto key : delta_.sibling_deletions)
    {
        const auto bad = static_cast<std::uint32_t>(key >> 32);
        const auto good = static_cast<std::uint32_t>(key & 0xffffffffu);
        const auto iterator = impl_->siblings.find(bad);
        if (iterator != impl_->siblings.end() && iterator->second == good)
        {
            impl_->siblings.erase(iterator);
        }
    }
    for (const auto& [child, ancestors] : delta_.parent_additions)
    {
        auto& target = impl_->parents[child];
        for (auto ancestor : ancestors)
        {
            if (std::find(target.begin(), target.end(), ancestor) == target.end())
            {
                target.push_back(ancestor);
            }
        }
    }
    for (const auto& key : delta_.parent_deletions)
    {
        const auto child = static_cast<std::uint32_t>(key >> 32);
        const auto parent = static_cast<std::uint32_t>(key & 0xffffffffu);
        auto iterator = impl_->parents.find(child);
        if (iterator != impl_->parents.end())
        {
            auto& list = iterator->second;
            list.erase(std::remove(list.begin(), list.end(), parent), list.end());
        }
    }

    open_ = true;
    return true;
}

std::optional<std::string> Store::tag_string(std::uint32_t tag_id) const
{
    const auto delta_iterator = delta_.tags.find(tag_id);
    if (delta_iterator != delta_.tags.end())
    {
        return delta_iterator->second;
    }

    for (auto iterator = impl_->tag_segments.rbegin(); iterator != impl_->tag_segments.rend(); ++iterator)
    {
        const auto& segment = *iterator;
        if (tag_id < segment.base || tag_id >= segment.base + segment.count)
        {
            continue;
        }

        const auto* ids = segment.ids.data();
        std::size_t low = 0;
        std::size_t high = static_cast<std::size_t>(segment.count);
        while (low < high)
        {
            const std::size_t middle = low + (high - low) / 2;
            const auto id = read_u32(ids + middle * 4);
            if (id < tag_id)
            {
                low = middle + 1;
            }
            else
            {
                high = middle;
            }
        }

        if (low >= segment.count || read_u32(ids + low * 4) != tag_id)
        {
            return std::nullopt;
        }

        const auto* offsets = segment.idx.data();
        const std::uint64_t begin = read_u64(offsets + low * 12);
        const std::uint32_t length = read_u32(offsets + low * 12 + 8);
        if (begin > segment.str.size() || length > segment.str.size() - begin)
        {
            return std::nullopt;
        }

        return std::string(
            reinterpret_cast<const char*>(segment.str.data() + begin),
            static_cast<std::size_t>(length));
    }

    return std::nullopt;
}

TagQueryResult Store::lookup(const Hash& hash) const
{
    TagQueryResult result;

    if (!open_)
    {
        return result;
    }

    const auto partition = partition_for(hash);
    const auto& files = impl_->partitions[partition];

    std::uint32_t hash_id = 0;
    bool found = false;

    {
        std::size_t low = 0;
        std::size_t high = files.hash_count;
        while (low < high)
        {
            const std::size_t middle = low + (high - low) / 2;
            const auto entry = entry_at(files.hash_entries, middle);
            if (entry.hash < hash)
            {
                low = middle + 1;
            }
            else
            {
                high = middle;
            }
        }

        if (low < files.hash_count)
        {
            const auto entry = entry_at(files.hash_entries, low);
            if (entry.hash == hash)
            {
                hash_id = entry.hash_id;
                found = true;
            }
        }
    }

    if (!found && partition < delta_.hashes_by_partition.size())
    {
        const auto& delta_hashes = delta_.hashes_by_partition[partition];
        const auto iterator = std::lower_bound(delta_hashes.begin(), delta_hashes.end(), hash,
            [](const HashEntry& entry, const Hash& value) { return entry.hash < value; });
        if (iterator != delta_hashes.end() && iterator->hash == hash)
        {
            hash_id = iterator->hash_id;
            found = true;
        }
    }

    if (!found || delta_.deleted_files.count(hash_id) != 0)
    {
        return result;
    }

    result.found = true;
    result.hash_id = hash_id;

    std::vector<std::uint32_t> tag_ids;

    if (files.map_count > 0)
    {
        std::size_t low = 0;
        std::size_t high = files.map_count;
        while (low < high)
        {
            const std::size_t middle = low + (high - low) / 2;
            const auto id = read_u32(files.map_hash_ids + middle * 4);
            if (id < hash_id)
            {
                low = middle + 1;
            }
            else
            {
                high = middle;
            }
        }

        if (low < files.map_count && read_u32(files.map_hash_ids + low * 4) == hash_id)
        {
            const std::uint64_t begin = read_u64(files.map_offsets + low * 8);
            const std::uint64_t end = read_u64(files.map_offsets + (low + 1) * 8);
            if (begin <= end && end <= files.map_tag_count)
            {
                tag_ids.reserve(static_cast<std::size_t>(end - begin));
                for (std::uint64_t i = begin; i < end; ++i)
                {
                    tag_ids.push_back(read_u32(files.map_tags + i * 4));
                }
            }
        }
    }

    if (const auto iterator = delta_.mapping_additions.find(hash_id); iterator != delta_.mapping_additions.end())
    {
        tag_ids.insert(tag_ids.end(), iterator->second.begin(), iterator->second.end());
    }

    if (!delta_.mapping_deletions.empty())
    {
        tag_ids.erase(std::remove_if(tag_ids.begin(), tag_ids.end(),
                          [this, hash_id](std::uint32_t tag_id) {
                              return delta_.mapping_deletions.count(pair_key(hash_id, tag_id)) != 0;
                          }),
            tag_ids.end());
    }

    std::sort(tag_ids.begin(), tag_ids.end());
    tag_ids.erase(std::unique(tag_ids.begin(), tag_ids.end()), tag_ids.end());

    // Display tags: resolve sibling chains, then add transitive parents.
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
    for (auto tag_id : tag_ids)
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

    auto resolve_all = [this](const std::vector<std::uint32_t>& ids, std::vector<std::string>& output) {
        output.reserve(ids.size());
        for (const auto id : ids)
        {
            const auto text = tag_string(id);
            if (text)
            {
                output.push_back(*text);
            }
        }
        std::sort(output.begin(), output.end());
        output.erase(std::unique(output.begin(), output.end()), output.end());
    };

    resolve_all(tag_ids, result.storage_tags);
    resolve_all(display_ids, result.display_tags);
    return result;
}

std::uint64_t Store::delta_hash_count() const
{
    std::uint64_t total = 0;
    for (const auto& partition : delta_.hashes_by_partition)
    {
        total += partition.size();
    }
    return total;
}

std::uint64_t Store::total_hash_count() const
{
    const auto added = delta_hash_count();
    const auto deleted = static_cast<std::uint64_t>(delta_.deleted_files.size());
    return manifest_.hash_count + added - std::min(deleted, manifest_.hash_count + added);
}

std::uint64_t Store::total_mapping_count() const
{
    const auto added = delta_.mapping_add_count;
    const auto deleted = delta_.mapping_delete_count;
    const auto base = manifest_.mapping_count + added;
    return base - std::min<std::uint64_t>(deleted, base);
}

std::uint64_t Store::total_tag_count() const
{
    return manifest_.tag_count + delta_.tags.size();
}

bool load_store_manifest(const std::filesystem::path& store_dir, StoreManifest& manifest, std::string& error)
{
    const auto path = store_dir / "manifest.json";
    if (!path_exists(path))
    {
        error = "store has not been built yet (" + path.string() + " is missing)";
        return false;
    }

    auto bytes = read_file(path, error);
    if (!bytes)
    {
        return false;
    }

    try
    {
        const auto root = nlohmann::json::parse(*bytes);
        manifest.format = root.value("format", kStoreFormatVersion);
        manifest.partitions = root.value("partitions", 16u);
        manifest.hash_count = root.value("hash_count", static_cast<std::uint64_t>(0));
        manifest.mapping_count = root.value("mapping_count", static_cast<std::uint64_t>(0));
        manifest.tag_count = root.value("tag_count", static_cast<std::uint64_t>(0));
        manifest.built_at = root.value("built_at", static_cast<std::int64_t>(0));
        manifest.tag_segments.clear();

        for (const auto& segment : root.at("tag_segments"))
        {
            StoreManifest::TagSegmentInfo info;
            info.base = segment.at("base").get<std::uint64_t>();
            info.count = segment.at("count").get<std::uint64_t>();
            info.last = segment.value("last", info.base + (info.count > 0 ? info.count - 1 : 0));
            info.idx_path = segment.at("idx").get<std::string>();
            info.str_path = segment.at("str").get<std::string>();
            info.ids_path = segment.value("ids", std::string());
            manifest.tag_segments.push_back(std::move(info));
        }

        return true;
    }
    catch (const std::exception& exception)
    {
        error = std::string("cannot parse store manifest: ") + exception.what();
        return false;
    }
}

bool save_store_manifest(const std::filesystem::path& store_dir, const StoreManifest& manifest, std::string& error)
{
    nlohmann::json root;
    root["format"] = manifest.format;
    root["partitions"] = manifest.partitions;
    root["hash_count"] = manifest.hash_count;
    root["mapping_count"] = manifest.mapping_count;
    root["tag_count"] = manifest.tag_count;
    root["built_at"] = manifest.built_at;
    root["tag_segments"] = nlohmann::json::array();

    for (const auto& segment : manifest.tag_segments)
    {
        nlohmann::json row;
        row["base"] = segment.base;
        row["count"] = segment.count;
        row["last"] = segment.last;
        row["idx"] = segment.idx_path;
        row["str"] = segment.str_path;
        row["ids"] = segment.ids_path;
        root["tag_segments"].push_back(std::move(row));
    }

    return write_file_atomic(store_dir / "manifest.json", root.dump(), error);
}

} // namespace hyd
