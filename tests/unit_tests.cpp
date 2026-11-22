#include "common/log.hpp"
#include "common/util.hpp"
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
