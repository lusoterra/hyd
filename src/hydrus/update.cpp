#include "hydrus/update.hpp"

#include <simdjson.h>

#include <limits>

namespace hyd {

namespace {

constexpr std::int64_t kContentTypeMappings = 0;
constexpr std::int64_t kContentTypeTagSiblings = 1;
constexpr std::int64_t kContentTypeTagParents = 2;
constexpr std::int64_t kContentTypeFiles = 3;

constexpr std::int64_t kContentUpdateAdd = 0;
constexpr std::int64_t kContentUpdateDelete = 1;

constexpr std::int64_t kDefinitionsTypeHashes = 0;
constexpr std::int64_t kDefinitionsTypeTags = 1;

std::uint32_t checked_id(std::int64_t value)
{
    if (value < 0 || value > std::numeric_limits<std::uint32_t>::max())
    {
        throw std::runtime_error("id out of range: " + std::to_string(value));
    }
    return static_cast<std::uint32_t>(value);
}

} // namespace

struct UpdateParser::Impl
{
    simdjson::dom::parser parser;
    Bytes text;
};

UpdateParser::UpdateParser()
    : impl_(new Impl())
{
}

UpdateParser::~UpdateParser()
{
    delete impl_;
}

bool UpdateParser::parse(std::span<const std::uint8_t> compressed, UpdateVisitor& visitor, UpdateKind& kind, std::string& error)
{
    kind = UpdateKind::unknown;
    rows_processed_ = 0;
    error.clear();

    if (!zlib_inflate(compressed, impl_->text, error))
    {
        error = "cannot inflate update: " + error;
        return false;
    }

    try
    {
        const std::string_view text(
            reinterpret_cast<const char*>(impl_->text.data()), impl_->text.size());
        const auto document = impl_->parser.parse(text);
        const auto root = document.get_array();

        if (root.size() < 3)
        {
            error = "update is not a serialisable tuple";
            return false;
        }

        const auto type = root.at(0).get_int64().value();
        if (type == static_cast<std::int64_t>(UpdateKind::definitions))
        {
            kind = UpdateKind::definitions;

            const auto info = root.at(2).get_array();
            for (const auto& definition_group : info)
            {
                const auto group = definition_group.get_array();
                if (group.size() < 2)
                {
                    continue;
                }

                const auto definition_type = group.at(0).get_int64().value();
                const auto definitions = group.at(1).get_array();

                for (const auto& definition : definitions)
                {
                    const auto pair = definition.get_array();
                    if (pair.size() < 2)
                    {
                        continue;
                    }

                    const auto id = checked_id(pair.at(0).get_int64().value());

                    if (definition_type == kDefinitionsTypeHashes)
                    {
                        const auto encoded = pair.at(1).get_string().value();
                        const auto hash = hash_from_hex(encoded);
                        if (hash)
                        {
                            visitor.on_hash_definition(id, *hash);
                            ++rows_processed_;
                        }
                        else if (const auto decoded = hex_decode(encoded); decoded && !decoded->empty())
                        {
                            // The PTR has a handful of legacy SHA-1/MD5 file
                            // hashes. They cannot be indexed alongside the
                            // SHA-256 hashes, so let the visitor skip them.
                            visitor.on_unsupported_hash_definition(id, encoded);
                        }
                        else
                        {
                            error = "invalid hash definition for hash id " + std::to_string(id);
                            return false;
                        }
                    }
                    else if (definition_type == kDefinitionsTypeTags)
                    {
                        visitor.on_tag_definition(id, pair.at(1).get_string().value());
                        ++rows_processed_;
                    }
                }
            }

            return true;
        }

        if (type != static_cast<std::int64_t>(UpdateKind::content))
        {
            error = "unknown update serialisable type " + std::to_string(type);
            return false;
        }

        kind = UpdateKind::content;

        const auto info = root.at(2).get_array();
        for (const auto& content_group : info)
        {
            const auto group = content_group.get_array();
            if (group.size() < 2)
            {
                continue;
            }

            const auto content_type = group.at(0).get_int64().value();
            const auto actions = group.at(1).get_array();

            for (const auto& action_group : actions)
            {
                const auto action_pair = action_group.get_array();
                if (action_pair.size() < 2)
                {
                    continue;
                }

                const auto action = action_pair.at(0).get_int64().value();
                const auto data = action_pair.at(1).get_array();

                const bool is_add = action == kContentUpdateAdd;
                const bool is_delete = action == kContentUpdateDelete;

                if (!is_add && !is_delete)
                {
                    continue;
                }

                if (content_type == kContentTypeMappings)
                {
                    for (const auto& mapping_row : data)
                    {
                        const auto mapping = mapping_row.get_array();
                        if (mapping.size() < 2)
                        {
                            continue;
                        }

                        const auto tag_id = checked_id(mapping.at(0).get_int64().value());
                        const auto hash_ids = mapping.at(1).get_array();

                        for (const auto& hash_id_value : hash_ids)
                        {
                            const auto hash_id = checked_id(hash_id_value.get_int64().value());
                            if (is_add)
                            {
                                visitor.on_mapping_add(tag_id, hash_id);
                            }
                            else
                            {
                                visitor.on_mapping_delete(tag_id, hash_id);
                            }
                            ++rows_processed_;
                        }
                    }
                }
                else if (content_type == kContentTypeFiles)
                {
                    if (is_add)
                    {
                        for (const auto& file_row : data)
                        {
                            const auto fields = file_row.get_array();
                            if (fields.size() < 1)
                            {
                                continue;
                            }
                            const auto hash_id = checked_id(fields.at(0).get_int64().value());
                            visitor.on_file_add(hash_id);
                            ++rows_processed_;
                        }
                    }
                    else
                    {
                        for (const auto& hash_id_value : data)
                        {
                            const auto hash_id = checked_id(hash_id_value.get_int64().value());
                            visitor.on_file_delete(hash_id);
                            ++rows_processed_;
                        }
                    }
                }
                else if (content_type == kContentTypeTagSiblings || content_type == kContentTypeTagParents)
                {
                    for (const auto& pair_row : data)
                    {
                        const auto pair = pair_row.get_array();
                        if (pair.size() < 2)
                        {
                            continue;
                        }

                        const auto first = checked_id(pair.at(0).get_int64().value());
                        const auto second = checked_id(pair.at(1).get_int64().value());

                        if (content_type == kContentTypeTagSiblings)
                        {
                            if (is_add)
                            {
                                visitor.on_sibling_add(first, second);
                            }
                            else
                            {
                                visitor.on_sibling_delete(first, second);
                            }
                        }
                        else
                        {
                            if (is_add)
                            {
                                visitor.on_parent_add(first, second);
                            }
                            else
                            {
                                visitor.on_parent_delete(first, second);
                            }
                        }

                        ++rows_processed_;
                    }
                }
            }
        }

        return true;
    }
    catch (const simdjson::simdjson_error& exception)
    {
        error = std::string("cannot parse update JSON: ") + exception.what();
        return false;
    }
    catch (const std::exception& exception)
    {
        error = std::string("cannot parse update: ") + exception.what();
        return false;
    }
}

} // namespace hyd
