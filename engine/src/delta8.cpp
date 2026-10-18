#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <vector>

#include "delta.h"

namespace vt {
namespace {

struct Rec {
    uint64_t start, count;
    uint32_t q, r;
};

uint32_t be32(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}

}  // namespace

// NewTek's op-8 variant (toaster_iff._delta8_newtek): one contiguous run of words per plane.
//
// The DLTA starts with a 12-byte record per plane: start (u32 words from the plane's top-left),
// count (u32 words), then count / 26 and count % 26 as u16 (loop counters of the Amiga's unrolled
// copy). The planes' data follow in order, `count` big-endian words each, and replace the words in
// that range - but filled from the END backwards (pre-decrement copy): first one word if the
// remainder is odd, then remainder / 2 longwords, then count / 26 blocks of 26 words, each block
// in forward order.
void applyDelta8(Planes& planes, std::span<const uint8_t> dl) {
    const size_t p = size_t(planes.count);
    if (dl.size() < 12 * p) throw std::runtime_error("op8: DLTA shorter than the plane records");
    std::vector<Rec> recs;
    for (size_t k = 0; k < p; ++k) {
        const uint8_t* b = dl.data() + 12 * k;
        recs.push_back({be32(b), be32(b + 4), (uint32_t(b[8]) << 8) | b[9], (uint32_t(b[10]) << 8) | b[11]});
    }
    for (const Rec& c : recs)
        if (c.count && (c.count / 26 != c.q || c.count % 26 != c.r))
            throw std::runtime_error("op8: header does not match the NewTek layout");

    size_t i = 12 * p;
    const size_t flatSize = size_t(planes.h) * size_t(planes.rowbytes) / 2;  // words per plane
    std::vector<uint8_t> dst;
    for (size_t pl = 0; pl < p; ++pl) {
        const Rec& rec = recs[pl];
        if (!rec.count) continue;
        if (rec.count * 2 > dl.size() - i) throw std::runtime_error("op8: word data past the DLTA");
        const size_t count = size_t(rec.count);
        const uint8_t* w = dl.data() + i;  // count big-endian words
        i += 2 * count;
        dst.assign(2 * count, 0);
        size_t pos = count, j = 0;
        if (rec.r & 1) {
            pos -= 1;
            std::memcpy(&dst[2 * pos], w + 2 * j, 2);
            j += 1;
        }
        for (uint32_t k = 0; k < rec.r / 2; ++k) {
            pos -= 2;
            std::memcpy(&dst[2 * pos], w + 2 * j, 4);
            j += 2;
        }
        for (uint32_t k = 0; k < rec.q; ++k) {
            pos -= 26;
            std::memcpy(&dst[2 * pos], w + 2 * j, 52);
            j += 26;
        }
        if (rec.start < flatSize) {
            const size_t end = size_t(std::min<uint64_t>(rec.start + rec.count, flatSize));
            std::memcpy(planes.plane(int(pl)) + 2 * size_t(rec.start), dst.data(), 2 * (end - size_t(rec.start)));
        }
    }
}

}  // namespace vt
