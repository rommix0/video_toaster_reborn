#pragma once
// Toaster framestores: FRAM croutons (the files DV1/DV2 stills load from) as raster frames, and the
// ToasterCG key pages (KEYF croutons), which load into DV1/DV2 the same way.
// The 16-plane composite and 24-plane RGB layouts are tools/re/FREEZE.md §4.
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "vt/raster.h"
#include "vt/timing.h"

namespace vt {

inline constexpr int kFramestoreW = 752, kFramestoreH = 480;
inline constexpr int kFramestoreY0 = 1;  // HARDWARE GUESS: raster line of file row 0

// Where a w x h still sits in the raster: x0 = (kRasterW - w) / 2, y0 = kFramestoreY0. Centred,
// not at x = 0 as CGKEY.md hints: approved by the owner by eye (2026-11-04).
Rect framestoreRect(int w = kFramestoreW, int h = kFramestoreH);

// Ruling 9: `path` is a framestore when its own CrUD says FRAM; or it has no CrUD and its sibling
// icon (<name>.i / .I) says FRAM, as the original reads a crouton's CrUD from its icon; or neither
// has a CrUD and loadFramestore accepts its picture (16-plane lanes 6/7 or 24-plane). False for a
// missing or unreadable file; an unreadable icon has no CrUD. That last case decodes the
// picture: it goes to *picture when given, so the caller need not load it again.
bool isFramestore(const std::filesystem::path& path, std::optional<Frame>* picture = nullptr);

// A FRAM crouton's picture as a raster frame (black outside framestoreRect). 16 planes with PLTP
// lanes 6/7: composite (FREEZE.md §4); 24 planes with lanes 8/9/10: RGB. Anything else, or a
// malformed file: nullopt and *whyNot. Sizes: w <= 768 and h <= 480, both even.
std::optional<Frame> loadFramestore(const std::filesystem::path&, std::string* whyNot = nullptr);

// SAVE's pop-up (FREEZE.md §3 "SAVE"): the whole still, or one field of it line-doubled.
enum class SaveFields { One, Four };

// Writes `path` (picture + icons + CrUD) and `path` + ".I" (icons + CrUD), creating the folder, in
// the layout of FREEZE.md §4. The picture is `still` inside framestoreRect(), written as the
// original's SAVE writes it (switcher.main 0x11ba66 flags 3): 16-plane composite, PLTP lanes 6/7,
// one DvQuad per pixel pair (docs/2026-11-04-save-fields-design.md). SaveFields::One first keeps
// file rows 1, 3 .. 477, makes rows 2 .. 476 the mean of their neighbours and rows 0, 478, 479 black.
// False and *whyNot on failure; no half-written file is left behind.
bool saveFramestore(const std::filesystem::path&, const Frame& still, SaveFields, std::string* whyNot = nullptr);

// A ToasterCG key page: a crouton of TYPE KEYF (4.2/CG/Keys; tools/re/CGKEY.md,
// docs/2026-10-30-cg-keys-design.md 2 A). Its FORM ILBM holds PLTP lanes 6/7 (composite, as a
// framestore) and lane 0x0b bits 1-0 (the alpha), BMHD y the page's first DV line.
struct CgKeyPage {
    std::filesystem::path path;  // the file it came from
    // The composite at raster lines kFramestoreY0 + y .. (x from 0, BMHD x ignored), black
    // elsewhere. The colour phase follows the DV line: lanes 6/7 trade when ((row + y + 1) >> 1) & 1.
    Frame fill;
    // kRasterW x kRasterH, row-major: the 2-bit alpha (0..3) placed as the fill, 0 elsewhere.
    std::vector<uint8_t> alpha;
    // The Auto speed values ($1f0) per speed button: algoTiming of the page's tags, i.e. the KEYF
    // defaults 90 / 60 / 30 / 60 and Medium (no shipped page has a speed tag; CGKEY.md section 2).
    Timing timing;
};

// TYPE KEYF in the file's own CrUD, or (with none) in its sibling icon's. False for a missing file.
bool isCgKeyPage(const std::filesystem::path&);

// nullopt and *whyNot for anything that is not such a page: w <= 768 and even, y >= 0, y + h <= 480.
std::optional<CgKeyPage> loadCgKeyPage(const std::filesystem::path&, std::string* whyNot = nullptr);

}  // namespace vt
