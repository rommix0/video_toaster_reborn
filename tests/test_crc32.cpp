#include "catch_amalgamated.hpp"
#include "vt/crc32.h"

#include <cstdint>
#include <string>

TEST_CASE("crc32 matches zlib") {
    const std::string s = "123456789";
    REQUIRE(vt::crc32({reinterpret_cast<const uint8_t*>(s.data()), s.size()}) == 0xCBF43926u);
    REQUIRE(vt::crc32({}) == 0u);
}
