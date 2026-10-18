#pragma once
#include <cstdint>
#include <span>

namespace vt {

// zlib-compatible CRC-32 (polynomial 0xEDB88320). `seed` is the CRC of the
// preceding data, so crc32(b, crc32(a)) == crc32(a ++ b).
uint32_t crc32(std::span<const uint8_t> data, uint32_t seed = 0);

}  // namespace vt
