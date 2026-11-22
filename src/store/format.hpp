#pragma once

#include "common/util.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>

namespace hyd {

constexpr std::uint32_t kStoreFormatVersion = 2;

constexpr char kHashesMagic[8] = { 'H', 'Y', 'D', 'H', 'A', 'S', 'H', '1' };
constexpr char kMapsMagic[8] = { 'H', 'Y', 'D', 'M', 'A', 'P', '0', '1' };
constexpr char kPairsMagic[8] = { 'H', 'Y', 'D', 'P', 'A', 'I', 'R', '1' };
constexpr char kLegacyMagic[8] = { 'H', 'Y', 'D', 'L', 'E', 'G', '0', '1' };

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
