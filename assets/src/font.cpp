#include "vt/assets/font.h"

#include <algorithm>
#include <cstdio>
#include <stdexcept>

namespace vt::assets {
namespace {

// Offsets in the size file's first hunk (hunk.py flat image): "moveq #0,d0; rts", then the DiskFontHeader
// (a Node, dfh_FileID, Revision, Segment, dfh_Name[32]) and the ColorTextFont (TextFont + colour part).
constexpr uint32_t kDfh = 4;
constexpr uint32_t kFileId = kDfh + 14, kName = kDfh + 22, kNameLen = 32, kTf = kDfh + 54;
constexpr uint16_t kDiskFontId = 0x0F80;
constexpr uint32_t kYSize = 20, kStyle = 22, kFlags = 23, kXSize = 24, kBaseline = 26, kLoChar = 32, kHiChar = 33,
                   kModulo = 38, kCharLoc = 40, kCharSpace = 44, kCharKern = 48, kCtfFlags = 52, kDepth = 54,
                   kColours = 60, kCharData = 64;
constexpr uint8_t kStyleColour = 0x40, kFlagProportional = 0x20;  // FSF_COLORFONT, FPF_PROPORTIONAL
constexpr uint16_t kCtColourFont = 1;                               // ctf_Flags CT_COLORFONT
constexpr int kMaxDepth = 8;

[[noreturn]] void bad(uint32_t addr, const std::string& why) {
    char a[16];
    std::snprintf(a, sizeof a, "0x%06x", addr);
    throw std::runtime_error("colour font: " + why + " (flat address " + a + ")");
}

// Bounds-checked reads that report the flat address instead of std::out_of_range.
class Reader {
public:
    explicit Reader(const HunkImage& h) : h_(h) {}
    uint8_t u8(uint32_t a) const {
        if (a >= h_.flat.size()) bad(a, "read past the end of the image");
        return h_.flat[a];
    }
    uint16_t u16(uint32_t a) const {
        try { return h_.be16(a); } catch (const std::out_of_range&) { bad(a, "read past the end of the image"); }
    }
    int16_t s16(uint32_t a) const { return int16_t(u16(a)); }
    uint32_t u32(uint32_t a) const {
        try { return h_.be32(a); } catch (const std::out_of_range&) { bad(a, "read past the end of the image"); }
    }

private:
    const HunkImage& h_;
};

// Glyph index of a character: outside lo..hi it is the last ("not found") glyph.
size_t glyph(const ColorFont& f, unsigned char ch) {
    const int i = int(ch) - f.loChar;
    return size_t(i >= 0 && i <= f.hiChar - f.loChar ? i : f.hiChar - f.loChar + 1);
}

}  // namespace

ColorFont loadColorFont(const HunkImage& sizeFile) {
    if (sizeFile.hunks.empty()) throw std::runtime_error("colour font: no hunks");
    const Reader rd(sizeFile);
    const uint32_t base = sizeFile.hunks[0].base;
    if (rd.u16(base + kFileId) != kDiskFontId) bad(base + kFileId, "not a DiskFontHeader (file id is not 0x0F80)");
    const uint32_t tf = base + kTf;
    if (!(rd.u8(tf + kStyle) & kStyleColour) || !(rd.u16(tf + kCtfFlags) & kCtColourFont))
        bad(tf + kStyle, "not a colour font");

    ColorFont f;
    for (uint32_t i = 0; i < kNameLen && rd.u8(base + kName + i); ++i) f.name += char(rd.u8(base + kName + i));
    f.ySize = rd.u16(tf + kYSize);
    f.baseline = rd.u16(tf + kBaseline);
    f.loChar = rd.u8(tf + kLoChar);
    f.hiChar = rd.u8(tf + kHiChar);
    f.depth = rd.u8(tf + kDepth);
    f.proportional = (rd.u8(tf + kFlags) & kFlagProportional) != 0;
    const int modulo = rd.u16(tf + kModulo);
    const int xSize = rd.u16(tf + kXSize);
    if (f.ySize == 0 || modulo == 0) bad(tf + kYSize, "empty strike");
    if (f.loChar > f.hiChar) bad(tf + kLoChar, "lo char above hi char");
    if (f.depth < 1 || f.depth > kMaxDepth) bad(tf + kDepth, "depth " + std::to_string(f.depth) + " is not 1..8");

    // ColorFontColors: reserved word, count, table of 12-bit colours.
    const uint32_t cfc = rd.u32(tf + kColours);
    const int count = rd.u16(cfc + 2);
    const uint32_t table = rd.u32(cfc + 4);
    for (int i = 0; i < count; ++i) {
        const uint16_t w = rd.u16(table + 2u * uint32_t(i));
        if (w > 0xfff) bad(table + 2u * uint32_t(i), "not a 12-bit colour");
        f.colours.push_back({uint8_t((w >> 8 & 15) * 17), uint8_t((w >> 4 & 15) * 17), uint8_t((w & 15) * 17), 255});
    }

    // Strike: ctf_CharData[p] is plane p, ySize rows of `modulo` bytes; plane p is bit p of the colour.
    const int sw = modulo * 8;
    f.strike = {sw, f.ySize, std::vector<uint8_t>(size_t(sw) * size_t(f.ySize), 0)};
    for (int p = 0; p < f.depth; ++p) {
        const uint32_t plane = rd.u32(tf + kCharData + 4u * uint32_t(p));
        rd.u8(plane + uint32_t(modulo) * uint32_t(f.ySize) - 1);  // the whole plane must be in the image
        for (int y = 0; y < f.ySize; ++y)
            for (int x = 0; x < sw; ++x)
                f.strike.px[size_t(y) * size_t(sw) + size_t(x)] |=
                    uint8_t((rd.u8(plane + uint32_t(y * modulo + x / 8)) >> (7 - x % 8) & 1) << p);
    }

    // Per-character tables: lo..hi plus the "not found" glyph.
    const uint32_t loc = rd.u32(tf + kCharLoc), space = rd.u32(tf + kCharSpace), kern = rd.u32(tf + kCharKern);
    const int n = f.hiChar - f.loChar + 2;
    for (int i = 0; i < n; ++i) {
        const uint32_t e = 4u * uint32_t(i);
        const int off = rd.u16(loc + e), w = rd.u16(loc + e + 2);
        if (off + w > sw) bad(loc + e, "glyph outside the strike");
        f.charLoc.push_back(off);
        f.charWidth.push_back(w);
        f.charSpace.push_back(space ? rd.s16(space + 2u * uint32_t(i)) : xSize);
        f.charKern.push_back(kern ? rd.s16(kern + 2u * uint32_t(i)) : 0);
    }
    return f;
}

int textWidth(const ColorFont& f, std::string_view text) {
    int w = 0;
    for (unsigned char ch : text) w += f.charSpace[glyph(f, ch)];
    return w;
}

void drawText(IndexedImage& dst, const ColorFont& f, int x, int y, std::string_view text, bool jam2, uint8_t bg) {
    const int top = y - f.baseline;
    for (unsigned char ch : text) {
        const size_t g = glyph(f, ch);
        const int off = f.charLoc[g], w = f.charWidth[g], kern = f.charKern[g], adv = f.charSpace[g];
        const int cols = std::max(w, adv - kern);
        for (int yy = 0; yy < f.ySize; ++yy) {
            const int Y = top + yy;
            if (Y < 0 || Y >= dst.h) continue;
            for (int xx = 0; xx < cols; ++xx) {
                const int X = x + kern + xx;
                if (X < 0 || X >= dst.w) continue;
                uint8_t c = xx < w ? f.strike.px[size_t(yy) * size_t(f.strike.w) + size_t(off + xx)] : 0;
                if (!c) {
                    if (!jam2) continue;
                    c = bg;
                }
                dst.px[size_t(Y) * size_t(dst.w) + size_t(X)] = c;
            }
        }
        x += adv;
    }
}

}  // namespace vt::assets
