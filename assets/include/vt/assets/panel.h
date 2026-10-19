#pragma once
// The Switcher panel of switcher.main: the pre-drawn 2-plane art of every button, label, header,
// pop-up and the T-bar, decoded from the "panel nodes" exactly as tools/ref/render_layout.py does it
// (../tools/re/SCREEN.md sections 2-4).
#include <cstdint>
#include <string>
#include <vector>

#include "vt/assets/font.h"
#include "vt/assets/hunk.h"
#include "vt/assets/indexed.h"
#include "vt/raster.h"

namespace vt::assets {

struct PanelNode {
    std::string id;                     // as in switcher_layout.json (e.g. "main_1", "take", "tbar")
    vt::Rect rect, hitRect;             // screen pixels: blit rect (+0x34, +0x2e, +0x30) and hit rect (+0x04)
    int method = 0;                     // draw method 0..5 (SCREEN.md section 3)
    std::vector<IndexedImage> states;   // state images (index = node's state value)
    std::vector<IndexedImage> masks;    // per state, 1 = inside the art (methods 4/5); empty otherwise
    IndexedImage image;                 // the current image (+0x38) at load; empty when there is none
    uint16_t selectPattern = 0;         // +0x4e when selected: the +0x4b byte in both bytes
    bool hiddenByDefault = false;       // +0x49
    int list = 0;                       // which of the nine lists ($158..$17c), -1 = not on a list
};

// Run-time texts drawn with the screen font (CGInterface 14) over the pop-ups.
struct PanelText {
    std::string id;                     // bkg_popup_text, speed_popup_text, key_level_text
    int x, y;                           // pen position: y is the baseline, or the text top when !baseline
    bool baseline = true;               // Move()+Text() (true) or PrintIText at the top-left (false)
    std::string text;                   // the default string ("" when only set at run time)
    bool hiddenByDefault = false;
};

struct PanelLayout {
    int screenW = 768, screenH = 481;
    vt::Rect projectArea{64, 44, 640, 200}, switcherArea{64, 244, 640, 240};
    Palette rgbMode, previewMode;          // 4 colours each
    std::vector<PanelNode> nodes;
    std::vector<IndexedImage> tbarImages;  // 86, index = position / 6
    vt::Rect tbarRect;
    std::vector<PanelText> texts;
    std::vector<std::string> bkgNames;     // the nine BKG pop-up strings, padded as stored ("Black  " ... "Snow   ")
    int clearColour = 1, clearMarginX = 8;
};

// Throws std::runtime_error naming the flat-image address on unexpected data.
PanelLayout loadPanel(const HunkImage& switcherMain);

// "Selected" look for methods 4/5: inside the mask 0->0, 1->2, 2->3, 3->1; methods 0-3: XOR both
// planes with selectPattern, rotated right by 2 on odd rows (SCREEN.md section 3).
IndexedImage selectedLook(const PanelNode&, int state);

// Draws the default (all nodes in state 0, none selected, hidden nodes skipped) Switcher screen as
// palette indices, screenW x screenH, without the run-time texts.
IndexedImage renderDefaultScreen(const PanelLayout&);

// The same with the visible run-time texts (default BKG name, key level) drawn in cgInterface14 (the
// CGInterface 14 colour font), JAM2 with background pen 0, as render_layout.py's texts=True render.
IndexedImage renderDefaultScreen(const PanelLayout&, const ColorFont& cgInterface14);

}  // namespace vt::assets
