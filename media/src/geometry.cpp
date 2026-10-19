#include "vt/media/geometry.h"

#include <cstdint>

namespace vt::media {
namespace {

// round(num / den) for positive operands.
int64_t divRound(int64_t num, int64_t den) { return (num + den / 2) / den; }

} // namespace

vt::Rect fitRect(int srcW, int srcH, AVRational srcSar, const MediaGeometry& g) {
    if (srcW <= 0 || srcH <= 0) return g.inputRect;
    if (srcSar.num <= 0 || srcSar.den <= 0) srcSar = {1, 1};
    const vt::Rect& box = g.inputRect;
    const AVRational pa = g.pixelAspect;
    // Display aspect DA = dn / dd; the box's is box.w * pa.num / (box.h * pa.den).
    const int64_t dn = int64_t{srcW} * srcSar.num, dd = int64_t{srcH} * srcSar.den;
    int w, h;
    if (dn * box.h * pa.den >= dd * box.w * pa.num) {
        w = box.w;
        h = static_cast<int>(divRound(int64_t{box.w} * pa.num * dd, int64_t{pa.den} * dn));
    } else {
        h = box.h;
        w = static_cast<int>(divRound(int64_t{box.h} * dn * pa.den, dd * pa.num));
    }
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    return {box.x + (box.w - w) / 2, box.y + (box.h - h) / 2, w, h};
}

} // namespace vt::media
