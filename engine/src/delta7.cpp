#include <cstring>
#include <stdexcept>

#include "delta.h"

namespace vt {

// NewTek's op-7 variant (toaster_iff._delta7_newtek).
//
// bits 0x01 = long data, 0x40 = byte data, otherwise word data; bits 0x10 = runs go down a column
// (the pointer steps a whole row), otherwise along a row. Each plane has its own stream, in plane
// order, addressed in units of the data size from the plane's top-left. A stream is a list of
// groups [count][offset:u16]: the 16-bit pointer moves by `offset`, then `count` literal values
// (or, if count < 0, one value repeated -count times) are written; count == -1 is a jump marker
// (pointer only); count == 0 ends the plane. Byte data uses an 8-bit signed count, word and long
// data a 16-bit one. Writes past the end of the plane are dropped.
void applyDelta7(Planes& planes, std::span<const uint8_t> dl, uint32_t bits) {
    const size_t ws = (bits & 1) ? 4 : (bits & 0x40) ? 1 : 2;
    const size_t csz = ws == 1 ? 1 : 2;
    const size_t rb = size_t(planes.rowbytes);
    const size_t L = dl.size();
    size_t i = 0;
    for (int pl = 0; pl < planes.count; ++pl) {
        if (rb % ws) throw std::runtime_error("op7: row is not a whole number of data units");  // numpy view()
        const size_t cols = rb / ws;
        const size_t n = size_t(planes.h) * cols;  // data units in the plane
        const size_t step = (bits & 0x10) ? cols : 1;
        uint8_t* base = planes.plane(pl);
        size_t ptr = 0;
        while (i + csz <= L) {
            const int cnt = ws == 1 ? int(int8_t(dl[i])) : int(int16_t((uint16_t(dl[i]) << 8) | dl[i + 1]));
            i += csz;
            if (cnt == 0) break;
            if (L - i < 2) throw std::runtime_error("op7: DLTA ended inside a group header");
            ptr = (ptr + ((size_t(dl[i]) << 8) | dl[i + 1])) & 0xFFFF;
            i += 2;
            if (cnt == -1) continue;  // jump marker: moves the pointer, no data
            const bool literal = cnt > 0;
            const size_t reps = size_t(literal ? cnt : -cnt);
            if ((literal ? reps * ws : ws) > L - i) throw std::runtime_error("op7: data run past the DLTA");
            for (size_t j = 0; j < reps; ++j) {
                const size_t pos = ptr + j * step;
                if (pos < n) std::memcpy(base + pos * ws, dl.data() + i + (literal ? j * ws : 0), ws);
            }
            i += literal ? reps * ws : ws;
            ptr += reps * step;
        }
    }
    if (L - i > 1) throw std::runtime_error("op7: unparsed bytes left in the DLTA");
}

}  // namespace vt
