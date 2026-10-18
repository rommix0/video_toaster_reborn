#include "vt/crc32.h"

#include <array>

namespace vt {
namespace {

constexpr std::array<uint32_t, 256> makeTable() {
    std::array<uint32_t, 256> t{};
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t c = i;
        for (int k = 0; k < 8; ++k) c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        t[i] = c;
    }
    return t;
}

constexpr auto kTable = makeTable();

}  // namespace

uint32_t crc32(std::span<const uint8_t> data, uint32_t seed) {
    uint32_t c = ~seed;
    for (uint8_t b : data) c = kTable[(c ^ b) & 0xFFu] ^ (c >> 8);
    return ~c;
}

}  // namespace vt
