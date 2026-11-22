#include "common/log.hpp"
#include "common/util.hpp"
#include "hydrus/repository.hpp"
#include "hydrus/update.hpp"
#include "store/delta.hpp"

#include <filesystem>
#include <fstream>
#include <unistd.h>
#include <zlib.h>

#include <cassert>
#include <iostream>

int main()
{
    using namespace hyd;

    {
        const auto hash = sha256("abc");
        assert(hex_encode(hash) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
        const auto parsed = hash_from_hex(hex_encode(hash));
        assert(parsed && *parsed == hash);
        assert(!hash_from_hex("abc"));
    }

    {
        // The PTR carries a few legacy SHA-1/MD5 hash definitions next to the
        // SHA-256 ones. They must be skipped, not parsed as corruption.
        const std::string sha256_hex(64, 'a');
        const std::string sha1_hex(40, 'b');
        const std::string json = "[36, 1, [[0, [[1, \"" + sha256_hex
            + "\"], [2, \"" + sha1_hex + "\"]]]]]";

        uLongf compressed_size = compressBound(json.size());
        Bytes compressed(compressed_size);
        assert(compress2(compressed.data(), &compressed_size,
                   reinterpret_cast<const Bytef*>(json.data()), json.size(), Z_BEST_SPEED) == Z_OK);
        compressed.resize(compressed_size);

        struct Visitor : UpdateVisitor
        {
            int supported = 0;
            int unsupported = 0;

            void on_hash_definition(std::uint32_t hash_id, const Hash& hash) override
            {
                assert(hash_id == 1);
                assert(hex_encode(hash) == std::string(64, 'a'));
                ++supported;
            }

            void on_unsupported_hash_definition(std::uint32_t hash_id, std::string_view encoded) override
            {
                assert(hash_id == 2);
                assert(encoded == std::string(40, 'b'));
                ++unsupported;
            }
        } visitor;

        UpdateParser parser;
        UpdateKind kind = UpdateKind::unknown;
        std::string parse_error;
        assert(parser.parse(compressed, visitor, kind, parse_error));
        assert(kind == UpdateKind::definitions);
        assert(visitor.supported == 1);
        assert(visitor.unsupported == 1);
    }

    {
        const auto sha1 = hash_bytes_from_hex(std::string(40, 'a'));
        assert(sha1 && sha1->size() == 20);
        const auto md5 = hash_bytes_from_hex(std::string(32, 'b'));
        assert(md5 && md5->size() == 16);
        const auto sha256_value = hash_bytes_from_hex(std::string(64, 'c'));
        assert(sha256_value && sha256_value->size() == 32);
        assert(!hash_bytes_from_hex(std::string(42, 'a')));
        assert(!hash_bytes_from_hex(std::string(40, 'z')));
    }

    {
        // Unknown types, versions, actions, and shapes must abort the parse
        // rather than silently dropping rows.
        auto compressed = [](std::string_view text) {
            uLongf compressed_size = compressBound(text.size());
            Bytes output(compressed_size);
            assert(compress2(output.data(), &compressed_size,
                       reinterpret_cast<const Bytef*>(text.data()), text.size(), Z_BEST_SPEED) == Z_OK);
            output.resize(compressed_size);
            return output;
        };

        struct NullVisitor : UpdateVisitor
        {
        };

        auto parse = [&](std::string_view json, UpdateVisitor& visitor) {
            auto bytes = compressed(json);
            UpdateParser parser;
            UpdateKind kind = UpdateKind::unknown;
            std::string parse_error;
            const bool ok = parser.parse(bytes, visitor, kind, parse_error);
            return std::pair<bool, std::string>(ok, parse_error);
        };

        NullVisitor null_visitor;

        auto result = parse(R"([36, 1, [[2, [[1, "aa"]]]]])", null_visitor);
        assert(!result.first && result.second.find("unknown definition type") != std::string::npos);

        result = parse(R"([34, 1, [[9, [[0, []]]]]])", null_visitor);
        assert(!result.first && result.second.find("unknown content type") != std::string::npos);

        result = parse(R"([34, 1, [[0, [[7, []]]]]])", null_visitor);
        assert(!result.first && result.second.find("unknown content update action") != std::string::npos);

        result = parse(R"([36, 2, [[0, []]]])", null_visitor);
        assert(!result.first && result.second.find("unsupported update serialisable version") != std::string::npos);

        const std::string odd_hash = "[36, 1, [[0, [[1, \"" + std::string(48, 'a') + "\"]]]]]";
        result = parse(odd_hash, null_visitor);
        assert(!result.first && result.second.find("unsupported 24-byte hash definition") != std::string::npos);

        struct InvalidVisitor : UpdateVisitor
        {
            int invalid = 0;
            void on_invalid_hash_definition(std::uint32_t, std::string_view) override { ++invalid; }
        } invalid_visitor;

        // One of the four known-invalid PTR rows is skipped; other invalid
        // lengths still fail (tested above).
        const std::string known_invalid = R"([36, 1, [[0, [[1, "0000"]]]]])";
        auto known_bytes = compressed(known_invalid);
        UpdateParser known_parser;
        {
            UpdateKind kind = UpdateKind::unknown;
            std::string parse_error;
            assert(known_parser.parse(known_bytes, invalid_visitor, kind, parse_error));
            assert(kind == UpdateKind::definitions);
            assert(invalid_visitor.invalid == 1);
        }

        result = parse(R"([34, 1, [[0, [[0]]]]])", null_visitor);
        assert(!result.first && result.second.find("malformed action row") != std::string::npos);

        // File adds are known, carry no tag data, and remain a valid no-op.
        struct FileVisitor : UpdateVisitor
        {
            int added = 0;
            void on_file_add(std::uint32_t) override { ++added; }
        } file_visitor;

        result = parse(R"([34, 1, [[3, [[0, [[5]]]]]]])", file_visitor);
        assert(result.first && file_visitor.added == 1);
    }

    {
        // Repository metadata versions are pinned too: silently missing a
        // metadata_slice would mean never downloading new updates.
        RepositoryMetadata metadata;
        std::string metadata_error;

        assert(parse_repository_metadata_json(
            R"([21, 2, [[[0, "metadata_slice"], [2, [37, 1, [[], 1800000000]]]]]])",
            metadata, metadata_error));

        assert(!parse_repository_metadata_json(
            R"([21, 3, [[[0, "metadata_slice"], [2, [37, 1, [[], 1800000000]]]]]])",
            metadata, metadata_error));
        assert(metadata_error.find("HydrusArgs version") != std::string::npos);

        assert(!parse_repository_metadata_json(
            R"([21, 2, [[[0, "metadata_slice"], [2, [37, 2, [[], 1800000000]]]]]])",
            metadata, metadata_error));
        assert(metadata_error.find("metadata_slice has unsupported version") != std::string::npos);
    }

    {
        const auto decoded = percent_decode("a%20b%2Fc+d");
        assert(decoded == "a b/c d");
    }

    {
        assert(parse_u64("0") == 0);
        assert(parse_u64("18446744073709551615").has_value());
        assert(!parse_u64("18446744073709551616").has_value());
        assert(parse_i64("-5") == -5);
    }

    {
        DeltaData data;
        data.init(4);
        assert(data.hashes_by_partition.size() == 4);
        assert(pair_key(1, 2) == ((1ull << 32) | 2));
    }

    {
        const auto directory = std::filesystem::temp_directory_path()
            / ("hyd-log-test-" + std::to_string(::getpid()));
        std::error_code ec;
        std::filesystem::remove_all(directory, ec);
        std::filesystem::create_directories(directory);

        const auto path = directory / "hyd.log";
        std::string log_error;
        log_set_level(LogLevel::info);
        assert(log_init_file(path.string(), 2000, 2, true, log_error));
        for (int i = 0; i < 200; ++i)
        {
            log_message(LogLevel::info, std::string(100, 'x') + std::to_string(i));
        }
        log_close_file();

        assert(std::filesystem::exists(path));
        assert(std::filesystem::exists(directory / "hyd.log.1.gz"));
        assert(std::filesystem::exists(directory / "hyd.log.2.gz"));
        assert(!std::filesystem::exists(directory / "hyd.log.3.gz"));

        std::ifstream gzip(directory / "hyd.log.1.gz", std::ios::binary);
        unsigned char magic[2] = {};
        gzip.read(reinterpret_cast<char*>(magic), sizeof(magic));
        assert(magic[0] == 0x1f && magic[1] == 0x8b);

        std::filesystem::remove_all(directory, ec);
        log_set_level(LogLevel::warn);
    }

    std::cout << "ok\n";
    return 0;
}
