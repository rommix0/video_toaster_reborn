#pragma once
// Crouton icons: the 80x50 ILBM pictures the Switcher's Project grid shows for the effect files of
// one drawer (4.2/Effects/**).
#include <filesystem>
#include <string>
#include <vector>

#include "vt/assets/indexed.h"

namespace vt::assets {

constexpr int kCroutonW = 80, kCroutonH = 50;

struct Crouton {
    std::filesystem::path effect, icon;  // icon == effect when the effect file carries its own picture
    std::string name;                    // the effect file's name (UTF-8)
    IndexedImage image;                  // 80x50
    Palette palette;                     // the icon's CMAP as stored (8 bits per gun, alpha 255)
};

// The croutons of `folder` (not its subfolders), sorted by file name (ASCII case-insensitive, then
// byte order). Every file other than an icon (name ending ".i" / ".I") is an effect file candidate:
// - its icon is the sibling "<name>.i" or "<name>.I"; else a sibling icon whose name matches after
//   both are lower-cased, stripped of trailing spaces and underscores and of the spaces before a
//   trailing "!" (the drawers hold "Border Fractal_" with "Border Fractal .i", "Spin Explode__" with
//   "Spin Explode.I", "Wipe Soft Circle Out!" with "Wipe Soft Circle Out !.I", "Wedding Bells !"
//   with "Wedding Bells!.I"); else the file's own leading FORM ILBM;
// - a file with neither is not a crouton and is skipped (IFF files that are skipped get a logged
//   reason, other files are skipped silently);
// - an icon that is not 80x50, has no CMAP or does not decode is refused with a logged reason;
// - a sibling icon with no ILBM at all (the ChromaFx ".I" files carry only the CrUD form; the effect
//   file itself starts with a 736x480 framestore) gets a plain placeholder picture: an 80x50 cell of
//   colour 2 with a colour-0 outline in the panel's grey 000 555 999 DDD. My call: the original
//   ChromaFX crouton picture was not found, and the effects stay reachable from the grid.
std::vector<Crouton> scanCroutons(const std::filesystem::path& folder);

}  // namespace vt::assets
