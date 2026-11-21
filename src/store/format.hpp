#pragma once

#include "common/util.hpp"

#include <cstdint>
#include <cstring>
#include <string>

namespace hyd {

constexpr std::uint32_t kStoreFormatVersion = 1;

constexpr char kHashesMagic[8] = { 'H', 'Y', 'D', 'H', 'A', 'S', 'H', '1' };
constexpr char kMapsMagic[8] = { 'H', 'Y', 'D', 'M', 'A', 'P', '0', '1' };
constexpr char kPairsMagic[8] = { 'H', 'Y', 'D', 'P', 'A', 'I', 'R', '1' };

constexpr std::uint8_t kDeltaTagDefinition = 1;
constexpr std::uint8_t kDeltaHashDefinition = 2;
constexpr std::uint8_t kDeltaMappingAdd = 3;
constexpr std::uint8_t kDeltaMappingDelete = 4;
constexpr std::uint8_t kDeltaFileDelete = 5;
constexpr std::uint8_t kDeltaSiblingAdd = 6;
constexpr std::uint8_t kDeltaSiblingDelete = 7;
constexpr std::uint8_t kDeltaParentAdd = 8;
constexpr std::uint8_t kDeltaParentDelete = 9;

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

} // namespace hyd
