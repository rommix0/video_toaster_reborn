#pragma once
// Amiga colour fonts (the Toaster's interface fonts): decoded from the diskfont size files
// ("NN.xC", a hunk executable holding a DiskFontHeader and a ColorTextFont) as
// tools/ref/amiga_font.py does it, plus the Text() renderer of the Switcher screen.
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "vt/assets/hunk.h"
#include "vt/assets/indexed.h"

namespace vt::assets {

struct ColorFont {
    std::string name;                 // dfh_Name as stored in the size file (the files carry the size string)
    int ySize = 0, baseline = 0, loChar = 0, hiChar = 0, depth = 1;
    Palette colours;                  // ColorFontColors (4-bit RGB expanded x17)
    IndexedImage strike;              // all planes combined into colour indices (plane p = bit p)
    // Per character lo..hi plus one last "not found" glyph, used for characters outside lo..hi.
    std::vector<int> charLoc;         // bit offset of the glyph in the strike
    std::vector<int> charWidth;       // glyph width in pixels
    std::vector<int> charSpace;       // pen advance (tf_XSize for every glyph when the font has no CharSpace table)
    std::vector<int> charKern;        // glyph offset from the pen (0 when the font has no CharKern table)
    bool proportional = false;        // FPF_PROPORTIONAL
};

// Throws std::runtime_error for anything that is not a well-formed colour font size file
// (DiskFontHeader file id 0x0F80, CT_COLORFONT), naming the flat-image address.
ColorFont loadColorFont(const HunkImage& sizeFile);

// Sum of the pen advances: the width of the cell run drawText() paints in JAM2.
int textWidth(const ColorFont&, std::string_view);

// Text(): draws text with its baseline at (x, y) into dst, clipped; characters are bytes.
// JAM1 leaves colour-0 glyph pixels alone; JAM2 paints them (and the rest of each advance cell) with bg.
void drawText(IndexedImage& dst, const ColorFont&, int x, int y, std::string_view, bool jam2, uint8_t bg = 0);

}  // namespace vt::assets
