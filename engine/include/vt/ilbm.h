#pragma once
// IFF ILBM stills: BMHD/CMAP/CAMG, ByteRun1, bit planes (port of tools/toaster_iff.py).
// Every function bounds-checks its input and throws std::runtime_error on malformed data.
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

#include "vt/iff.h"

namespace vt {

struct Rgb {
    uint8_t r, g, b;
};

struct Bitmap {
    int w = 0, h = 0, planes = 0, compression = 0, masking = 0;
    uint32_t camg = 0;
    std::vector<Rgb> cmap;  // empty when the ILBM has no CMAP
    int rowbytes() const { return ((w + 15) / 16) * 2; }
};

// Bit planes laid out like numpy (count, h, rowbytes).
struct Planes {
    int count = 0, h = 0, rowbytes = 0;
    std::vector<uint8_t> bytes;
    uint8_t* plane(int p) { return bytes.data() + size_t(p) * size_t(h) * size_t(rowbytes); }
    const uint8_t* plane(int p) const { return bytes.data() + size_t(p) * size_t(h) * size_t(rowbytes); }
};

// Palette indices, one byte per pixel, row-major (h rows of w).
struct IndexImage {
    int w = 0, h = 0;
    std::vector<uint8_t> px;
};

// Decode exactly outSize bytes. Throws if the source ends first (including a run cut off
// mid-way) or a run would write past outSize. Source bytes after outSize is reached are ignored.
std::vector<uint8_t> unpackByteRun1(std::span<const uint8_t> src, size_t outSize);

// ByteRun1 for one row: literal runs of at most 128 bytes, repeat runs of 3 to 128.
std::vector<uint8_t> packByteRun1(std::span<const uint8_t> row);

// BODY -> bit planes (de-interleaves rows, drops the mask plane). Throws on a short BODY.
Planes bodyToPlanes(const Bitmap& bm, std::span<const uint8_t> body);

// Bit planes -> indices cropped to w pixels. Version 1 holds indices in a byte, so more than
// 8 planes (the one 9-plane DVE ANIM) throws std::runtime_error; so does w > rowbytes * 8.
IndexImage planesToIndex(const Planes& planes, int w);

// BMHD, CMAP and CAMG of one FORM ILBM (BMHD required; BODY is not looked at).
Bitmap readBitmapHeader(std::span<const uint8_t> d, const Form& form);

// Decode one FORM ILBM (BMHD and BODY required).
std::pair<Bitmap, IndexImage> readIlbm(std::span<const uint8_t> d, const Form& form);

}  // namespace vt
