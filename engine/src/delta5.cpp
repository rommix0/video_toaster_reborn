#include <algorithm>
#include <stdexcept>

#include "delta.h"

namespace vt {

// Op 5: byte vertical delta (toaster_iff._delta5). 16 big-endian plane pointers (only the first
// 8 are used), each addressing one stream with one run list per byte column.
void applyDelta5(Planes& planes, std::span<const uint8_t> dl) {
    if (dl.size() < 64) throw std::runtime_error("op5: DLTA shorter than the pointer table");
    const size_t rb = size_t(planes.rowbytes), h = size_t(planes.h);
    auto at = [&](size_t i) -> uint8_t {
        if (i >= dl.size()) throw std::runtime_error("op5: DLTA ended inside a column");
        return dl[i];
    };
    for (int p = 0; p < std::min(planes.count, 8); ++p) {
        const uint32_t ptr = (uint32_t(dl[4 * p]) << 24) | (uint32_t(dl[4 * p + 1]) << 16) |
                             (uint32_t(dl[4 * p + 2]) << 8) | dl[4 * p + 3];
        if (!ptr) continue;
        size_t i = ptr;
        uint8_t* pl = planes.plane(p);
        for (size_t col = 0; col < rb; ++col) {
            const size_t nops = at(i++);
            size_t y = 0;
            for (size_t k = 0; k < nops; ++k) {
                const uint8_t op = at(i++);
                if (op == 0) {
                    const size_t cnt = at(i);
                    const uint8_t val = at(i + 1);
                    i += 2;
                    for (size_t r = y; r < std::min(y + cnt, h); ++r) pl[r * rb + col] = val;
                    y += cnt;
                } else if (op & 0x80) {
                    const size_t cnt = op & 0x7f;
                    if (cnt > dl.size() - i) throw std::runtime_error("op5: literal run past the DLTA");
                    // numpy: the slice is clamped to the plane; a shorter slice only accepts one value
                    // (broadcast), anything else is a shape mismatch.
                    const size_t room = y < h ? std::min(cnt, h - y) : 0;
                    if (room == cnt) {
                        for (size_t r = 0; r < cnt; ++r) pl[(y + r) * rb + col] = dl[i + r];
                    } else if (cnt != 1) {
                        throw std::runtime_error("op5: literal run past the bottom of the plane");
                    }
                    i += cnt;
                    y += cnt;
                } else {
                    y += op;
                }
            }
        }
    }
}

}  // namespace vt
