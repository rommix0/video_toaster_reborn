#pragma once
// ToasterCG scroll and crawl pages: croutons of TYPE SCRO (4.2/CG/Scrolls) and CRAW (4.2/CG/Crawls),
// played over Main by the Switcher (docs/2026-10-30-cg-scrolls-design.md, ../tools/re/CGSCROLL.md).
// The file holds a glyph strip (FORM ILBM, 2-bit coverage) and FORM MVPG (glyph, placement and line
// records); loadCgScroll renders it as switcher.main does (cgscroll_ref.py, bit-exact) into the rows the
// page shows, and works out its per-field motion, once, at load.
#include <array>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "vt/raster.h"

namespace vt {

struct CgScroll {
    enum class Kind { Scroll, Crawl };
    std::filesystem::path path;  // the file it came from
    Kind kind = Kind::Scroll;
    int speed = 0;  // as played: tag 0x8000006a (the MVPG speed byte without it); scroll 0..4, crawl 0..3 (4 = 3)
    // A scroll's end (CGSCROLL.md section 3): pause (tag 0x80000087 != 0) stops at stopField, holds `hold`
    // fields (tag >> 1), then fades over 22 fields at levels 255, 243, ... 3; else it runs once.
    bool pause = false;
    int hold = 0;
    int stopField = -1;  // pause: the first field after the motion stopped; -1 otherwise
    int endField = 0;    // the first field with the page off (Main alone)
    // The 8 Amiga colours as RGB (12-bit, nibble * 17): 0 background (transparent), 1-3 face, 4 edge,
    // 5 / 6 edge-to-face blends, 7 face (cgscroll_ref.palette).
    std::array<Rgba8, 8> palette{};
    // Amiga colour indices. Scroll: width 640, `rows` rows, every row the scroll feeds, in order (row m of
    // the stream; rows past the end are 0). Crawl: the continuous strip, `width` = 768 x windows wide and
    // `rows` tall (the crawl band's rows), stored window by window: strip column U, row r is
    // px[(U / 768 * rows + r) * 768 + U % 768].
    int width = 0, rows = 0;
    std::vector<uint8_t> px;
    // Scroll: tops[f] = rows fed after field f (f = 0 the first); field j shows tops[j - 1] (0 for j = 0),
    // the last one again after the motion stopped. Frame line Y (0..475) shows stream row top + Y - 480.
    std::vector<int> tops;
    // Crawl: field j shows strip column x + pxPerField * j - 768 at raster column x (0..767), strip row r
    // on frame line bandTop + r.
    int pxPerField = 0, bandTop = 0;
};

// The pre-rendered page's largest size: a corrupt page is refused once its rows pass it (the shipped
// pages need 0.2 to 6.2 MB).
inline constexpr size_t kCgScrollMaxBytes = size_t(64) << 20;

// TYPE SCRO or CRAW in the file's own CrUD. False for a missing or unreadable file.
bool isCgScroll(const std::filesystem::path&);

// The page, played at `speed` (-1: its own, tag 0x8000006a). nullopt and *whyNot for anything that is
// not such a page or does not decode (a record past the end of the strip, a placement past the records,
// a motion that never ends, a pre-render over kCgScrollMaxBytes).
std::optional<CgScroll> loadCgScroll(const std::filesystem::path&, std::string* whyNot = nullptr, int speed = -1);

}  // namespace vt
