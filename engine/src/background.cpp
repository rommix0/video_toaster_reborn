#include "vt/background.h"

#include <array>

#include "vt/dv_colour.h"

namespace vt {
namespace {

// switcher.main's digital matte table at 0x6d88 ($222(a5)) holds these colours as DV YIQ
// (bytes 1..3 of each entry). Converted by dvYiqToRgb (the inverse ChromaFX RGBYIQ matrix,
// DV black 71 / white 200 -> 0 / 255; tools/re/FINDINGS.md). Index = BkgColour - 1.
constexpr std::array<DvYiq, 8> kYiq{{
    {71, 0, 0},      // Black   -> 0, 0, 0
    {200, 0, 0},     // White   -> 255, 255, 255
    {100, 58, 20},   // Red     -> 191, 1, 0
    {128, -27, -51}, // Green   -> 0, 192, 0
    {81, -31, 30},   // Blue    -> 0, 0, 189
    {157, 31, -30},  // Yellow  -> 192, 192, 1
    {110, 27, 51},   // Magenta -> 191, 0, 190
    {138, -58, -20}, // Cyan    -> 0, 189, 192
}};

const std::array<Rgba8, 8> kColour = [] {
    std::array<Rgba8, 8> c{};
    for (size_t i = 0; i < c.size(); ++i) c[i] = dvYiqToRgb(kYiq[i]);
    return c;
}();

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

namespace {

uint64_t mix64(uint64_t z) {  // splitmix64 finaliser
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
}

}  // namespace

void renderSnow(Frame& f, uint32_t frameNumber) {
    // Seed from a full mix of the frame number so consecutive frames are independent streams
    // (a linear seed made frame n+1 equal frame n shifted by one pixel).
    uint64_t s = mix64(uint64_t(frameNumber) ^ 0x5bd1e9955bd1e995ull);
    for (auto& p : f.px) {  // splitmix64, one grey byte per pixel
        const auto v = uint8_t(mix64(s += 0x9e3779b97f4a7c15ull) >> 56);
        p = {v, v, v, 255};
    }
}

}  // namespace vt
