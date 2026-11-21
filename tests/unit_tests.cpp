#include "common/log.hpp"
#include "common/util.hpp"
#include "store/delta.hpp"

#include <filesystem>
#include <fstream>
#include <unistd.h>

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
