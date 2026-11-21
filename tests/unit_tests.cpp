#include "common/util.hpp"
#include "store/delta.hpp"

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

    std::cout << "ok\n";
    return 0;
}
