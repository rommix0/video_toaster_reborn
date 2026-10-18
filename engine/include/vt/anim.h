#pragma once
// IFF ANIM decoding: op 0 (full BODY), op 5 (byte vertical), NewTek op 7 and op 8
// (port of toaster_iff.read_anim, _delta5, _delta7_newtek, _delta8_newtek).
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "vt/iff.h"
#include "vt/ilbm.h"

namespace vt {

struct Anim {
    Bitmap bm;                                       // header, size, depth and palette of frame 0
    std::vector<Planes> planes;                      // bit planes of every decoded frame
    // Palette indices per frame. Left EMPTY when bm.planes > 8 (the one 9-plane DVE): version 1
    // holds indices in a byte, so such effects are used through `planes` only.
    std::vector<IndexImage> frames;
    std::vector<std::optional<std::vector<Rgb>>> cmaps;  // per-frame CMAP; frame 0: bm.cmap if it has one
    std::vector<int> reltimes;                       // frame 0 is 0
    int dropped = 0;                                 // damaged trailing frames left out
};

// Decode a FORM ANIM. Frame n is a delta from frame n - interleave (ANHD byte 18, 0 -> 2), or from
// frame n - 1 while n < interleave. Throws std::runtime_error on undecodable data, except that a
// delta op failing in one of the last two sub-forms drops that frame and everything after it
// (counted in `dropped`) instead of throwing. Unsupported ops throw after the loop, as in Python.
Anim readAnim(std::span<const uint8_t> d, const Form& form);

}  // namespace vt
