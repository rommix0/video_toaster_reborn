#include "vt/background.h"

#include <array>

namespace vt {
namespace {

// switcher.main's digital matte table at 0x6d88 ($222(a5)) holds these colours as DV YIQ
// (bytes 1..3 of each entry). Converted with the inverse ChromaFX RGBYIQ matrix (0x4938),
// scaled so DV black 71 / white 200 map to 0 / 255, then rounded and clamped. The
// derivation is in tools/re/FINDINGS.md. Index = BkgColour - 1.
constexpr std::array<Rgba8, 8> kColour{{
    {0, 0, 0, 255},        // Black    Y 71  I 0   Q 0
    {255, 255, 255, 255},  // White    Y 200 I 0   Q 0
    {191, 1, 0, 255},      // Red      Y 100 I 58  Q 20
    {0, 192, 0, 255},      // Green    Y 128 I -27 Q -51
    {0, 0, 189, 255},      // Blue     Y 81  I -31 Q 30
    {192, 192, 1, 255},    // Yellow   Y 157 I 31  Q -30
    {191, 0, 190, 255},    // Magenta  Y 110 I 27  Q 51
    {0, 189, 192, 255},    // Cyan     Y 138 I -58 Q -20
}};

// The generator's hardware words for the same colours (button list at 0x99000, word at +$4c).
constexpr std::array<uint16_t, 8> kHwWord{0x0000, 0x0bb0, 0x0906, 0x00ba, 0x0071, 0x0d51, 0x090a, 0x02d6};

}  // namespace

Rgba8 bkgColour(BkgColour c) {
    const int i = int(c) - 1;
    if (i < 0 || i >= int(kColour.size())) return {0, 0, 0, 255};  // Snow or out of range
    return kColour[size_t(i)];
}

Rgba8 customBkgColour(uint32_t tag0x24) {
    const uint16_t w = uint16_t(tag0x24 & 0x0fff);
    if (w == 0x0fff) return kColour[1];  // the Switcher's "super white" for White (0xe7276)
    for (size_t i = 0; i < kHwWord.size(); ++i)
        if (kHwWord[i] == w) return kColour[i];
    // Not defined by the program (see background.h): approximate as 0x0RGB.
    return {uint8_t((w >> 8 & 15) * 17), uint8_t((w >> 4 & 15) * 17), uint8_t((w & 15) * 17), 255};
}

void renderSnow(Frame& f, uint32_t frameNumber) {
    uint64_t s = 0x9e3779b97f4a7c15ull * (uint64_t(frameNumber) + 1);
    for (auto& p : f.px) {  // splitmix64, one grey byte per pixel
        uint64_t z = (s += 0x9e3779b97f4a7c15ull);
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
        const auto v = uint8_t((z ^ (z >> 31)) >> 56);
        p = {v, v, v, 255};
    }
}

}  // namespace vt
