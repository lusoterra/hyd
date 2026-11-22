#pragma once

#include "common/util.hpp"

#include <cstdint>
#include <string_view>

namespace hyd {

enum class UpdateKind
{
    unknown = 0,
    content = 34,
    definitions = 36,
};

class UpdateVisitor
{
public:
    virtual ~UpdateVisitor() = default;

    virtual void on_hash_definition(std::uint32_t hash_id, const Hash& hash) { (void)hash_id; (void)hash; }
    // The PTR also carries a few legacy SHA-1 (and possibly MD5) file hashes.
    // The mirror is keyed by SHA-256 only, so those definitions are reported
    // here instead of being treated as corrupt data.
    virtual void on_unsupported_hash_definition(std::uint32_t hash_id, std::string_view encoded_hash)
    {
        (void)hash_id;
        (void)encoded_hash;
    }
    // Only called when the parser is configured with
    // allow_invalid_hashes = true. These values are not any Hydrus hash type
    // and can never be queried.
    virtual void on_invalid_hash_definition(std::uint32_t hash_id, std::string_view encoded_hash)
    {
        (void)hash_id;
        (void)encoded_hash;
    }
    virtual void on_tag_definition(std::uint32_t tag_id, std::string_view tag) { (void)tag_id; (void)tag; }

    virtual void on_mapping_add(std::uint32_t tag_id, std::uint32_t hash_id) { (void)tag_id; (void)hash_id; }
    virtual void on_mapping_delete(std::uint32_t tag_id, std::uint32_t hash_id) { (void)tag_id; (void)hash_id; }

    virtual void on_file_add(std::uint32_t hash_id) { (void)hash_id; }
    virtual void on_file_delete(std::uint32_t hash_id) { (void)hash_id; }

    virtual void on_sibling_add(std::uint32_t bad_tag_id, std::uint32_t good_tag_id) { (void)bad_tag_id; (void)good_tag_id; }
    virtual void on_sibling_delete(std::uint32_t bad_tag_id, std::uint32_t good_tag_id) { (void)bad_tag_id; (void)good_tag_id; }

    virtual void on_parent_add(std::uint32_t child_tag_id, std::uint32_t parent_tag_id) { (void)child_tag_id; (void)parent_tag_id; }
    virtual void on_parent_delete(std::uint32_t child_tag_id, std::uint32_t parent_tag_id) { (void)child_tag_id; (void)parent_tag_id; }
};

class UpdateParser
{
public:
    UpdateParser();
    ~UpdateParser();

    UpdateParser(const UpdateParser&) = delete;
    UpdateParser& operator=(const UpdateParser&) = delete;

    // compressed is the zlib-compressed network bytes as served by the
    // repository. kind is set even when parsing fails after the header.
    bool parse(std::span<const std::uint8_t> compressed, UpdateVisitor& visitor, UpdateKind& kind, std::string& error);

    // A few PTR rows are not valid hashes at all. By default they are fatal;
    // this opt-in reports them through on_invalid_hash_definition() and skips
    // them instead.
    void set_allow_invalid_hashes(bool allow) { allow_invalid_hashes_ = allow; }

    std::uint64_t rows_processed() const { return rows_processed_; }

private:
    struct Impl;
    Impl* impl_;
    std::uint64_t rows_processed_ = 0;
    bool allow_invalid_hashes_ = false;
};

} // namespace hyd
