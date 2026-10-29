#pragma once
// Toaster framestores: FRAM croutons (the files DV1/DV2 stills load from) as raster frames.
// The 16-plane composite and 24-plane RGB layouts are tools/re/FREEZE.md §4.
#include <filesystem>
#include <optional>
#include <string>

#include "vt/raster.h"

namespace vt {

inline constexpr int kFramestoreW = 752, kFramestoreH = 480;
inline constexpr int kFramestoreY0 = 1;  // HARDWARE GUESS: raster line of file row 0

// Where a w x h still sits in the raster: x0 = (kRasterW - w) / 2, y0 = kFramestoreY0.
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

// Writes `path` (picture + icons + CrUD) and `path` + ".I" (icons + CrUD), creating the folder, in
// the layout of FREEZE.md §4. The picture is `still` inside framestoreRect(), 24-plane RGB with the
// Switcher's own BMHD (switcher.main 0x11ca4c). False and *whyNot on failure; no half-written file
// is left behind.
bool saveFramestore(const std::filesystem::path&, const Frame& still, std::string* whyNot = nullptr);

}  // namespace vt
