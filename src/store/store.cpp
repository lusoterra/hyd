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

LegacyHashEntry legacy_entry_at(const std::uint8_t* base, std::size_t index)
{
    LegacyHashEntry entry;
    std::memcpy(&entry, base + index * sizeof(LegacyHashEntry), sizeof(entry));
    return entry;
}

std::uint32_t partition_of(std::span<const std::uint8_t> hash, std::size_t partitions)
{
    if (hash.empty())
    {
        return 0;
    }
    return (static_cast<std::uint32_t>(hash[0]) * static_cast<std::uint32_t>(partitions)) / 256;
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

    MappedFile legacy;
    const std::uint8_t* legacy_entries = nullptr;
    std::uint32_t legacy_count = 0;

    MappedFile counts;
    const std::uint8_t* count_entries = nullptr;
    std::uint32_t count_len = 0;
    std::int64_t counts_built_at = 0;

    MappedFile tag_lookup;
    const std::uint8_t* lookup_entries = nullptr;
    std::uint32_t lookup_count = 0;

    MappedFile postings;
    const std::uint8_t* postings_offsets = nullptr;
    const std::uint8_t* postings_ids = nullptr;
    std::uint32_t postings_tag_count = 0;
    std::uint64_t postings_total = 0;

    MappedFile hash_index;
    const std::uint8_t* hash_index_entries = nullptr;
    std::uint64_t hash_index_count = 0;

    // Pending-delta adjustments to the base counts (tag id -> -N..+N).
    std::unordered_map<std::uint32_t, std::int32_t> count_overlay;

    std::unordered_map<std::uint32_t, std::uint32_t> siblings;
    std::unordered_map<std::uint32_t, std::vector<std::uint32_t>> parents;

    const PartitionFiles* find_partition_with_hash(std::uint32_t hash_id) const
    {
        for (const auto& files : partitions)
        {
            const std::uint8_t* ids = files.map_hash_ids;
            std::size_t low = 0;
            std::size_t high = files.map_count;
            while (low < high)
            {
                const std::size_t middle = low + (high - low) / 2;
                if (read_u32(ids + middle * 4) < hash_id)
                {
                    low = middle + 1;
                }
                else
                {
                    high = middle;
                }
            }
            if (low < files.map_count && read_u32(ids + low * 4) == hash_id)
            {
                return &files;
            }
        }
        return nullptr;
    }

    void collect_base_tags(std::uint32_t hash_id, std::vector<std::uint32_t>& tags) const
    {
        tags.clear();
        const auto* files = find_partition_with_hash(hash_id);
        if (files == nullptr)
        {
            return;
        }
        const std::uint8_t* ids = files->map_hash_ids;
        std::size_t low = 0;
        std::size_t high = files->map_count;
        while (low < high)
        {
            const std::size_t middle = low + (high - low) / 2;
            if (read_u32(ids + middle * 4) < hash_id)
            {
                low = middle + 1;
            }
            else
            {
                high = middle;
            }
        }
        if (low >= files->map_count)
        {
            return;
        }
        const std::uint64_t begin = read_u64(files->map_offsets + low * 8);
        const std::uint64_t end = read_u64(files->map_offsets + (low + 1) * 8);
        if (begin > end || end > files->map_tag_count)
        {
            return;
        }
        tags.reserve(static_cast<std::size_t>(end - begin));
        for (std::uint64_t i = begin; i < end; ++i)
        {
            tags.push_back(read_u32(files->map_tags + i * 4));
        }
    }

    void build_count_overlay(const DeltaData& delta)
    {
        count_overlay.clear();
        if (count_entries == nullptr)
        {
            return;
        }

        auto adjust = [this](std::uint32_t tag_id, std::int32_t change) {
            count_overlay[tag_id] += change;
        };

        // Files deleted in the delta still have their base mappings until the
        // next compaction; the query path hides the file, so the counts must
        // hide its tags too.
        for (const auto hash_id : delta.deleted_files)
        {
            std::vector<std::uint32_t> base;
            collect_base_tags(hash_id, base);
            for (const auto tag_id : base)
            {
                adjust(tag_id, -1);
            }
        }

        std::unordered_map<std::uint32_t, std::vector<std::uint32_t>> deletions;
        for (const auto key : delta.mapping_deletions)
        {
            deletions[static_cast<std::uint32_t>(key >> 32)].push_back(
                static_cast<std::uint32_t>(key & 0xffffffffu));
        }

        std::unordered_set<std::uint32_t> affected;
        for (const auto& [hash_id, tags] : delta.mapping_additions)
        {
            (void)tags;
            affected.insert(hash_id);
        }
        for (const auto& [hash_id, tags] : deletions)
        {
            (void)tags;
            affected.insert(hash_id);
        }

        for (const auto hash_id : affected)
        {
            if (delta.deleted_files.count(hash_id) != 0)
            {
                continue;
            }

            std::vector<std::uint32_t> base;
            collect_base_tags(hash_id, base);
            std::vector<std::uint32_t> final = base;

            auto contains = [](const std::vector<std::uint32_t>& values, std::uint32_t value) {
                return std::find(values.begin(), values.end(), value) != values.end();
            };

            if (const auto iterator = delta.mapping_additions.find(hash_id);
                iterator != delta.mapping_additions.end())
            {
                for (const auto tag_id : iterator->second)
                {
                    if (!contains(final, tag_id))
                    {
                        final.push_back(tag_id);
                    }
                }
            }
            if (const auto iterator = deletions.find(hash_id); iterator != deletions.end())
            {
                for (const auto tag_id : iterator->second)
                {
                    final.erase(std::remove(final.begin(), final.end(), tag_id), final.end());
                }
            }

            // Only tags touched by the delta can change their membership.
            std::unordered_set<std::uint32_t> candidates;
            if (const auto iterator = delta.mapping_additions.find(hash_id);
                iterator != delta.mapping_additions.end())
            {
                candidates.insert(iterator->second.begin(), iterator->second.end());
            }
            if (const auto iterator = deletions.find(hash_id); iterator != deletions.end())
            {
                candidates.insert(iterator->second.begin(), iterator->second.end());
            }

            for (const auto tag_id : candidates)
            {
                const bool in_base = contains(base, tag_id);
                const bool in_final = contains(final, tag_id);
                if (in_base != in_final)
                {
                    adjust(tag_id, in_final ? 1 : -1);
                }
            }
        }
    }
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
    return partition_for(std::span<const std::uint8_t>(hash.data(), hash.size()));
}

std::uint32_t Store::partition_for(std::span<const std::uint8_t> hash) const
{
    const auto parts = std::max<std::size_t>(1, manifest_.partitions);
    return partition_of(hash, parts);
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

    impl_->legacy_entries = nullptr;
    impl_->legacy_count = 0;
    impl_->legacy = MappedFile();
    const auto legacy_path = store_dir / "legacy.bin";
    if (path_exists(legacy_path))
    {
        if (!impl_->legacy.open(legacy_path, error))
        {
            return false;
        }
        if (impl_->legacy.size() < 12)
        {
            error = "legacy hash index is truncated: " + legacy_path.string();
            return false;
        }
        const auto* legacy_data = impl_->legacy.data();
        if (std::memcmp(legacy_data, kLegacyMagic, 8) != 0)
        {
            error = "bad legacy hash index magic: " + legacy_path.string();
            return false;
        }
        impl_->legacy_count = read_u32(legacy_data + 8);
        if (impl_->legacy.size()
            < 12 + static_cast<std::size_t>(impl_->legacy_count) * sizeof(LegacyHashEntry))
        {
            error = "legacy hash index has an invalid count: " + legacy_path.string();
            return false;
        }
        impl_->legacy_entries = legacy_data + 12;
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

    impl_->counts = MappedFile {};
    impl_->count_entries = nullptr;
    impl_->count_len = 0;
    impl_->counts_built_at = 0;

    {
        const auto path = store_dir / "counts.bin";
        if (path_exists(path))
        {
            std::string counts_error;
            if (!impl_->counts.open(path, counts_error))
            {
                HYD_LOGF(warn, "ignoring tag counts: %s", counts_error.c_str());
            }
            else if (impl_->counts.size() < kTagCountsHeader
                || std::memcmp(impl_->counts.data(), kTagCountsMagic, 8) != 0
                || read_u32(impl_->counts.data() + 8) != kTagCountsFormat)
            {
                HYD_LOG(warn, "ignoring tag counts: bad header (run --rebuild-tag-counts)");
            }
            else
            {
                const std::uint32_t count = read_u32(impl_->counts.data() + 12);
                const std::uint64_t generation = read_u64(impl_->counts.data() + 16);
                if (generation != manifest_.generation)
                {
                    HYD_LOG(warn,
                        "ignoring tag counts: they belong to an older store generation "
                        "(run --rebuild-tag-counts)");
                }
                else if (impl_->counts.size() < kTagCountsHeader + static_cast<std::size_t>(count) * 4)
                {
                    HYD_LOG(warn, "ignoring tag counts: truncated (run --rebuild-tag-counts)");
                }
                else
                {
                    impl_->count_entries = impl_->counts.data() + kTagCountsHeader;
                    impl_->count_len = count;
                    impl_->counts_built_at = static_cast<std::int64_t>(
                        read_u64(impl_->counts.data() + 24));
                }
            }
        }
    }

    impl_->tag_lookup = MappedFile {};
    impl_->lookup_entries = nullptr;
    impl_->lookup_count = 0;

    {
        const auto path = store_dir / "tag_lookup.bin";
        if (path_exists(path))
        {
            std::string lookup_error;
            if (!impl_->tag_lookup.open(path, lookup_error))
            {
                HYD_LOGF(warn, "ignoring tag name index: %s", lookup_error.c_str());
            }
            else if (impl_->tag_lookup.size() < kTagLookupHeader
                || std::memcmp(impl_->tag_lookup.data(), kTagLookupMagic, 8) != 0
                || read_u32(impl_->tag_lookup.data() + 8) != kTagLookupFormat)
            {
                HYD_LOG(warn,
                    "ignoring tag name index: bad header (run --rebuild-tag-counts)");
            }
            else
            {
                const std::uint32_t count = read_u32(impl_->tag_lookup.data() + 12);
                const std::uint64_t generation = read_u64(impl_->tag_lookup.data() + 16);
                if (generation != manifest_.generation)
                {
                    HYD_LOG(warn,
                        "ignoring tag name index: it belongs to an older store generation "
                        "(run --rebuild-tag-counts)");
                }
                else if (impl_->tag_lookup.size() < kTagLookupHeader + static_cast<std::size_t>(count) * 8)
                {
                    HYD_LOG(warn,
                        "ignoring tag name index: truncated (run --rebuild-tag-counts)");
                }
                else
                {
                    impl_->lookup_entries = impl_->tag_lookup.data() + kTagLookupHeader;
                    impl_->lookup_count = count;
                }
            }
        }
    }

    impl_->postings = MappedFile {};
    impl_->postings_offsets = nullptr;
    impl_->postings_ids = nullptr;
    impl_->postings_tag_count = 0;
    impl_->postings_total = 0;

    {
        const auto path = store_dir / "tag_postings.bin";
        if (path_exists(path))
        {
            std::string postings_error;
            if (!impl_->postings.open(path, postings_error))
            {
                HYD_LOGF(warn, "ignoring tag postings: %s", postings_error.c_str());
            }
            else if (impl_->postings.size() < kTagPostingsHeader
                || std::memcmp(impl_->postings.data(), kTagPostingsMagic, 8) != 0
                || read_u32(impl_->postings.data() + 8) != kTagPostingsFormat)
            {
                HYD_LOG(warn,
                    "ignoring tag postings: bad header (run --rebuild-tag-counts)");
            }
            else
            {
                const std::uint32_t tag_count = read_u32(impl_->postings.data() + 12);
                const std::uint64_t generation = read_u64(impl_->postings.data() + 16);
                const std::uint64_t total = read_u64(impl_->postings.data() + 24);
                const std::uint64_t expected = kTagPostingsHeader
                    + (static_cast<std::uint64_t>(tag_count) + 1) * 8 + total * 4;
                if (generation != manifest_.generation)
                {
                    HYD_LOG(warn,
                        "ignoring tag postings: they belong to an older store generation "
                        "(run --rebuild-tag-counts)");
                }
                else if (impl_->postings.size() < expected)
                {
                    HYD_LOG(warn,
                        "ignoring tag postings: truncated (run --rebuild-tag-counts)");
                }
                else
                {
                    impl_->postings_offsets = impl_->postings.data() + kTagPostingsHeader;
                    impl_->postings_ids = impl_->postings_offsets
                        + (static_cast<std::size_t>(tag_count) + 1) * 8;
                    impl_->postings_tag_count = tag_count;
                    impl_->postings_total = total;
                }
            }
        }
    }

    impl_->hash_index = MappedFile {};
    impl_->hash_index_entries = nullptr;
    impl_->hash_index_count = 0;

    {
        const auto path = store_dir / "hash_index.bin";
        if (path_exists(path))
        {
            std::string index_error;
            if (!impl_->hash_index.open(path, index_error))
            {
                HYD_LOGF(warn, "ignoring hash id index: %s", index_error.c_str());
            }
            else if (impl_->hash_index.size() < kHashIndexHeader
                || std::memcmp(impl_->hash_index.data(), kHashIndexMagic, 8) != 0
                || read_u32(impl_->hash_index.data() + 8) != kHashIndexFormat)
            {
                HYD_LOG(warn,
                    "ignoring hash id index: bad header (run --rebuild-tag-counts)");
            }
            else
            {
                const std::uint64_t count = read_u32(impl_->hash_index.data() + 12);
                const std::uint64_t generation = read_u64(impl_->hash_index.data() + 16);
                if (generation != manifest_.generation)
                {
                    HYD_LOG(warn,
                        "ignoring hash id index: it belongs to an older store generation "
                        "(run --rebuild-tag-counts)");
                }
                else if (impl_->hash_index.size() < kHashIndexHeader + count * 8)
                {
                    HYD_LOG(warn,
                        "ignoring hash id index: truncated (run --rebuild-tag-counts)");
                }
                else
                {
                    impl_->hash_index_entries = impl_->hash_index.data() + kHashIndexHeader;
                    impl_->hash_index_count = count;
                }
            }
        }
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

    impl_->build_count_overlay(delta_);

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

bool Store::has_tag_counts() const
{
    return open_ && impl_->count_entries != nullptr && impl_->lookup_entries != nullptr;
}

std::int64_t Store::tag_counts_built_at() const
{
    return impl_->counts_built_at;
}

std::optional<std::uint32_t> Store::resolve_tag_id(std::string_view tag) const
{
    std::optional<std::uint32_t> tag_id;

    if (impl_->lookup_entries != nullptr)
    {
        const std::uint32_t wanted = tag_name_hash(tag);
        std::size_t low = 0;
        std::size_t high = impl_->lookup_count;
        while (low < high)
        {
            const std::size_t middle = low + (high - low) / 2;
            if (read_u32(impl_->lookup_entries + middle * 8) < wanted)
            {
                low = middle + 1;
            }
            else
            {
                high = middle;
            }
        }
        while (low < impl_->lookup_count
            && read_u32(impl_->lookup_entries + low * 8) == wanted)
        {
            const auto candidate = read_u32(impl_->lookup_entries + low * 8 + 4);
            if (const auto text = tag_string(candidate); text && *text == tag)
            {
                tag_id = candidate;
                break;
            }
            ++low;
        }
    }

    if (!tag_id)
    {
        // A tag defined since the last compaction only exists in the delta
        // journal and is not in the lookup file yet.
        for (const auto& [candidate, text] : delta_.tags)
        {
            if (text == tag)
            {
                tag_id = candidate;
                break;
            }
        }
    }

    return tag_id;
}

std::optional<std::uint64_t> Store::tag_count(std::string_view tag) const
{
    if (!open_ || impl_->count_entries == nullptr)
    {
        return std::nullopt;
    }

    const auto tag_id = resolve_tag_id(tag);
    if (!tag_id)
    {
        return std::nullopt;
    }

    std::int64_t count = 0;
    if (static_cast<std::size_t>(*tag_id) < impl_->count_len)
    {
        count = read_u32(impl_->count_entries + static_cast<std::size_t>(*tag_id) * 4);
    }
    if (const auto iterator = impl_->count_overlay.find(*tag_id);
        iterator != impl_->count_overlay.end())
    {
        count += iterator->second;
    }

    return static_cast<std::uint64_t>(std::max<std::int64_t>(0, count));
}

bool Store::has_tag_hashes() const
{
    return open_ && impl_->count_entries != nullptr && impl_->lookup_entries != nullptr
        && impl_->postings_ids != nullptr && impl_->hash_index_entries != nullptr;
}

std::optional<TagHashesPage> Store::tag_hashes(std::string_view tag,
    std::uint64_t offset, std::uint32_t limit, std::optional<std::uint32_t> after) const
{
    if (!has_tag_hashes())
    {
        return std::nullopt;
    }

    const auto tag_id = resolve_tag_id(tag);
    if (!tag_id || *tag_id >= impl_->postings_tag_count)
    {
        return std::nullopt;
    }

    const std::uint64_t begin = read_u64(impl_->postings_offsets + static_cast<std::size_t>(*tag_id) * 8);
    const std::uint64_t end = read_u64(
        impl_->postings_offsets + (static_cast<std::size_t>(*tag_id) + 1) * 8);
    if (begin > end || end > impl_->postings_total)
    {
        return std::nullopt;
    }

    const auto total = tag_count(tag);
    if (!total)
    {
        return std::nullopt;
    }

    std::unordered_set<std::uint32_t> removed;
    for (const auto key : delta_.mapping_deletions)
    {
        const auto hash_id = static_cast<std::uint32_t>(key >> 32);
        const auto deleted_tag = static_cast<std::uint32_t>(key & 0xffffffffu);
        if (deleted_tag == *tag_id && delta_.deleted_files.count(hash_id) == 0)
        {
            removed.insert(hash_id);
        }
    }

    std::vector<std::uint32_t> added;
    for (const auto& [hash_id, tags] : delta_.mapping_additions)
    {
        if (delta_.deleted_files.count(hash_id) != 0 || removed.count(hash_id) != 0)
        {
            continue;
        }
        if (std::find(tags.begin(), tags.end(), *tag_id) != tags.end())
        {
            added.push_back(hash_id);
        }
    }
    std::sort(added.begin(), added.end());
    added.erase(std::unique(added.begin(), added.end()), added.end());

    auto resolve_hash = [this](std::uint32_t hash_id) -> std::optional<std::string> {
        if (static_cast<std::uint64_t>(hash_id) >= impl_->hash_index_count)
        {
            return std::nullopt;
        }
        const std::uint64_t entry = read_u64(
            impl_->hash_index_entries + static_cast<std::size_t>(hash_id) * 8);
        if (entry == 0)
        {
            return std::nullopt;
        }
        const auto partition = static_cast<std::uint32_t>(entry >> 32);
        const auto index = static_cast<std::uint64_t>(entry & 0xffffffffu) - 1;

        if (partition == 0xFFFFFFFFu)
        {
            if (index >= impl_->legacy_count)
            {
                return std::nullopt;
            }
            LegacyHashEntry legacy = legacy_entry_at(impl_->legacy_entries, static_cast<std::size_t>(index));
            return hex_encode(std::span<const std::uint8_t>(legacy.hash.data(), legacy.length));
        }

        if (partition >= impl_->partitions.size())
        {
            return std::nullopt;
        }
        const auto& files = impl_->partitions[partition];
        if (index >= files.hash_count)
        {
            return std::nullopt;
        }
        const auto* entry_bytes = files.hash_entries + static_cast<std::size_t>(index) * sizeof(HashEntry);
        return hex_encode(std::span<const std::uint8_t>(entry_bytes, kHashSize));
    };

    std::vector<std::string> page;
    page.reserve(std::min<std::uint64_t>(limit, 4096));

    std::size_t base_index = static_cast<std::size_t>(begin);
    std::size_t add_index = 0;

    if (after)
    {
        // Base postings are sorted by hash id, so a cursor page starts at the
        // first larger id instead of walking from the beginning.
        std::size_t low = static_cast<std::size_t>(begin);
        std::size_t high = static_cast<std::size_t>(end);
        while (low < high)
        {
            const std::size_t middle = low + (high - low) / 2;
            if (read_u32(impl_->postings_ids + middle * 4) <= *after)
            {
                low = middle + 1;
            }
            else
            {
                high = middle;
            }
        }
        base_index = low;
        add_index = static_cast<std::size_t>(
            std::upper_bound(added.begin(), added.end(), *after) - added.begin());
    }

    std::uint64_t seen = 0;
    bool has_more = false;
    std::optional<std::uint32_t> last_value;

    auto next_base = [&]() -> std::optional<std::uint32_t> {
        while (base_index < end)
        {
            const auto hash_id = read_u32(impl_->postings_ids + base_index * 4);
            ++base_index;
            if (delta_.deleted_files.count(hash_id) != 0 || removed.count(hash_id) != 0)
            {
                continue;
            }
            return hash_id;
        }
        return std::nullopt;
    };

    auto base_value = next_base();
    std::optional<std::uint32_t> add_value;
    if (add_index < added.size())
    {
        add_value = added[add_index++];
    }

    while (base_value || add_value)
    {
        std::uint32_t value = 0;
        if (base_value && (!add_value || *base_value < *add_value))
        {
            value = *base_value;
            base_value = next_base();
        }
        else if (add_value && (!base_value || *add_value < *base_value))
        {
            value = *add_value;
            add_value = add_index < added.size()
                ? std::optional<std::uint32_t>(added[add_index++])
                : std::nullopt;
        }
        else
        {
            value = *base_value;
            base_value = next_base();
            add_value = add_index < added.size()
                ? std::optional<std::uint32_t>(added[add_index++])
                : std::nullopt;
        }

        if (seen < offset)
        {
            ++seen;
            continue;
        }
        if (page.size() >= limit)
        {
            has_more = true;
            break;
        }

        const auto hash = resolve_hash(value);
        if (!hash)
        {
            return std::nullopt;
        }
        page.push_back(*hash);
        last_value = value;
        ++seen;
    }

    TagHashesPage result;
    result.total = *total;
    result.offset = offset;
    result.has_more = has_more;
    result.next_offset = offset + page.size();
    if (has_more && last_value)
    {
        result.next_after = *last_value;
    }
    result.hashes = std::move(page);
    return result;
}

TagQueryResult Store::lookup(std::span<const std::uint8_t> hash) const
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

    if (hash.size() == kHashSize)
    {
        Hash value {};
        std::copy(hash.begin(), hash.end(), value.begin());

        std::size_t low = 0;
        std::size_t high = files.hash_count;
        while (low < high)
        {
            const std::size_t middle = low + (high - low) / 2;
            const auto entry = entry_at(files.hash_entries, middle);
            if (entry.hash < value)
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
            if (entry.hash == value)
            {
                hash_id = entry.hash_id;
                found = true;
            }
        }

        if (!found && partition < delta_.hashes_by_partition.size())
        {
            const auto& delta_hashes = delta_.hashes_by_partition[partition];
            const auto iterator = std::lower_bound(delta_hashes.begin(), delta_hashes.end(), value,
                [](const HashEntry& entry, const Hash& lookup) { return entry.hash < lookup; });
            if (iterator != delta_hashes.end() && iterator->hash == value)
            {
                hash_id = iterator->hash_id;
                found = true;
            }
        }
    }
    else
    {
        const auto matches = [&hash](const LegacyHashEntry& entry) {
            if (entry.length != hash.size())
            {
                return false;
            }
            return std::equal(hash.begin(), hash.end(), entry.hash.begin());
        };

        const auto less = [](const LegacyHashEntry& entry, std::span<const std::uint8_t> value) {
            if (entry.length != value.size())
            {
                return entry.length < value.size();
            }
            return std::lexicographical_compare(entry.hash.begin(), entry.hash.begin() + entry.length,
                value.begin(), value.end());
        };

        std::size_t low = 0;
        std::size_t high = impl_->legacy_count;
        while (low < high)
        {
            const std::size_t middle = low + (high - low) / 2;
            const auto entry = legacy_entry_at(impl_->legacy_entries, middle);
            if (less(entry, hash))
            {
                low = middle + 1;
            }
            else
            {
                high = middle;
            }
        }

        if (low < impl_->legacy_count && matches(legacy_entry_at(impl_->legacy_entries, low)))
        {
            hash_id = legacy_entry_at(impl_->legacy_entries, low).hash_id;
            found = true;
        }

        if (!found)
        {
            for (const auto& entry : delta_.legacy_hashes)
            {
                if (matches(entry))
                {
                    hash_id = entry.hash_id;
                    found = true;
                    break;
                }
            }
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
    std::uint64_t total = delta_.legacy_hashes.size();
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

std::uint64_t Store::total_sibling_count() const
{
    return open_ ? static_cast<std::uint64_t>(impl_->siblings.size()) : 0;
}

std::uint64_t Store::total_parent_count() const
{
    if (!open_)
    {
        return 0;
    }

    std::uint64_t total = 0;
    for (const auto& [child, ancestors] : impl_->parents)
    {
        (void)child;
        total += ancestors.size();
    }
    return total;
}

std::uint64_t Store::legacy_hash_count() const
{
    if (!open_)
    {
        return 0;
    }
    return static_cast<std::uint64_t>(impl_->legacy_count) + delta_.legacy_hashes.size();
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
        if (manifest.partitions < 1 || manifest.partitions > 64
            || (manifest.partitions & (manifest.partitions - 1)) != 0)
        {
            error = "store manifest has an invalid partition count "
                + std::to_string(manifest.partitions);
            return false;
        }
        manifest.hash_count = root.value("hash_count", static_cast<std::uint64_t>(0));
        manifest.mapping_count = root.value("mapping_count", static_cast<std::uint64_t>(0));
        manifest.tag_count = root.value("tag_count", static_cast<std::uint64_t>(0));
        manifest.generation = root.value("generation", static_cast<std::uint64_t>(0));
        manifest.built_at = root.value("built_at", static_cast<std::int64_t>(0));
        manifest.tag_segments.clear();

        auto valid_segment_path = [](const std::string& p, bool allow_empty) {
            if (p.empty())
            {
                return allow_empty;
            }
            if (p.size() > 256 || p.front() == '/' || p.find("..") != std::string::npos
                || p.find('\0') != std::string::npos)
            {
                return false;
            }
            return true;
        };

        for (const auto& segment : root.at("tag_segments"))
        {
            StoreManifest::TagSegmentInfo info;
            info.base = segment.at("base").get<std::uint64_t>();
            info.count = segment.at("count").get<std::uint64_t>();
            info.last = segment.value("last", info.base + (info.count > 0 ? info.count - 1 : 0));
            info.idx_path = segment.at("idx").get<std::string>();
            info.str_path = segment.at("str").get<std::string>();
            info.ids_path = segment.value("ids", std::string());
            if (!valid_segment_path(info.idx_path, false)
                || !valid_segment_path(info.str_path, false)
                || !valid_segment_path(info.ids_path, true))
            {
                error = "store manifest contains an unsafe tag segment path";
                return false;
            }
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
    root["generation"] = manifest.generation;
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
