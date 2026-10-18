#pragma once
// 8SVX effect sounds (port of find_8svx / read_8svx in tools/toaster_iff.py).
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace vt {

struct Sound {
    int rate = 0;
    int channels = 0;           // 1 or 2
    std::vector<int16_t> pcm;   // interleaved; 8-bit samples scaled by 256
};

// The first FORM 8SVX anywhere in the file, or nullopt if there is none. Throws
// std::runtime_error for a malformed or compressed (unsupported) 8SVX.
std::optional<Sound> readSound(std::span<const uint8_t> file);

}  // namespace vt
