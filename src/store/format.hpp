#pragma once

#include "common/util.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

namespace hyd {

constexpr std::uint32_t kStoreFormatVersion = 2;

constexpr char kHashesMagic[8] = { 'H', 'Y', 'D', 'H', 'A', 'S', 'H', '1' };
constexpr char kMapsMagic[8] = { 'H', 'Y', 'D', 'M', 'A', 'P', '0', '1' };
constexpr char kPairsMagic[8] = { 'H', 'Y', 'D', 'P', 'A', 'I', 'R', '1' };
constexpr char kLegacyMagic[8] = { 'H', 'Y', 'D', 'L', 'E', 'G', '0', '1' };
constexpr char kTagCountsMagic[8] = { 'H', 'Y', 'D', 'C', 'N', 'T', '0', '1' };
constexpr char kTagLookupMagic[8] = { 'H', 'Y', 'D', 'T', 'I', 'X', '0', '1' };
constexpr char kTagPostingsMagic[8] = { 'H', 'Y', 'D', 'P', 'O', 'S', 'T', '1' };
constexpr char kHashIndexMagic[8] = { 'H', 'Y', 'D', 'H', 'I', 'D', 'X', '1' };

// counts.bin: magic, format, tag_count, generation (u64), built_at (u64),
// then one u32 current-mapping count per tag id. 4 bytes per defined tag id.
constexpr std::uint32_t kTagCountsFormat = 2;
constexpr std::size_t kTagCountsHeader = 32;

// tag_lookup.bin: magic, format, entry_count, generation (u64), then pairs of
// (u32 fnv1a(tag text), u32 tag id) sorted by the hash. 8 bytes per defined
// tag id; the tag id is verified against the tag dictionary on a hit.
constexpr std::uint32_t kTagLookupFormat = 2;
constexpr std::size_t kTagLookupHeader = 24;

// tag_postings.bin: magic, format, tag_count, generation (u64),
// total_postings (u64), built_at (u64), then (tag_count + 1) u64 offsets,
// then total_postings u32 hash ids. Each tag's hash ids are sorted ascending.
// 4 bytes per mapping plus 8 bytes per defined tag.
constexpr std::uint32_t kTagPostingsFormat = 2;
constexpr std::size_t kTagPostingsHeader = 40;

// hash_index.bin: magic, format, count, generation (u64), then one u64 per
// hash id: 0 = absent, otherwise (partition << 32) | (entry_index + 1).
// 8 bytes per defined hash. Lets a posting page resolve ids to hashes in O(1).
constexpr std::uint32_t kHashIndexFormat = 2;
constexpr std::size_t kHashIndexHeader = 24;

constexpr std::uint8_t kDeltaTagDefinition = 1;
constexpr std::uint8_t kDeltaHashDefinition = 2;
constexpr std::uint8_t kDeltaMappingAdd = 3;
constexpr std::uint8_t kDeltaMappingDelete = 4;
constexpr std::uint8_t kDeltaFileDelete = 5;
constexpr std::uint8_t kDeltaSiblingAdd = 6;
constexpr std::uint8_t kDeltaSiblingDelete = 7;
constexpr std::uint8_t kDeltaParentAdd = 8;
constexpr std::uint8_t kDeltaParentDelete = 9;
constexpr std::uint8_t kDeltaLegacyHashDefinition = 10;

inline void write_u32(std::uint8_t* destination, std::uint32_t value)
{
    std::memcpy(destination, &value, sizeof(value));
}

inline void write_u64(std::uint8_t* destination, std::uint64_t value)
{
    std::memcpy(destination, &value, sizeof(value));
}

inline std::uint32_t read_u32(const std::uint8_t* source)
{
    std::uint32_t value = 0;
    std::memcpy(&value, source, sizeof(value));
    return value;
}

inline std::uint64_t read_u64(const std::uint8_t* source)
{
    std::uint64_t value = 0;
    std::memcpy(&value, source, sizeof(value));
    return value;
}

struct HashEntry
{
    Hash hash {};
    std::uint32_t hash_id = 0;
};

static_assert(sizeof(HashEntry) == 36);

inline bool hash_entry_less(const HashEntry& left, const HashEntry& right)
{
    return left.hash < right.hash;
}

// The PTR's hash table is not exclusively SHA-256: a few entries are legacy
// SHA-1 (20 byte) or MD5 (16 byte) values. This record keeps them in a side
// index so their tags survive, keyed by the padded hash bytes.
struct LegacyHashEntry
{
    std::uint8_t length = 0;
    std::uint8_t padding[3] = {};
    Hash hash {};
    std::uint32_t hash_id = 0;
};

static_assert(sizeof(LegacyHashEntry) == 40);

inline bool legacy_hash_entry_less(const LegacyHashEntry& left, const LegacyHashEntry& right)
{
    if (left.length != right.length)
    {
        return left.length < right.length;
    }
    return left.hash < right.hash;
}

// 32-bit FNV-1a over the tag text. Used only as a fast first-level key for
// the tag-name lookup; every hit is verified against the tag dictionary, so
// collisions cost an extra comparison and never a wrong answer.
inline std::uint32_t tag_name_hash(std::string_view text)
{
    std::uint32_t hash = 2166136261u;
    for (const unsigned char byte : text)
    {
        hash ^= byte;
        hash *= 16777619u;
    }
    return hash;
}

inline LegacyHashEntry make_legacy_hash_entry(std::uint32_t hash_id,
    std::span<const std::uint8_t> hash)
{
    LegacyHashEntry entry;
    entry.length = static_cast<std::uint8_t>(hash.size());
    entry.hash_id = hash_id;
    std::copy(hash.begin(), hash.end(), entry.hash.begin());
    return entry;
}

} // namespace hyd
