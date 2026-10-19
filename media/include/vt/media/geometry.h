#pragma once
// Where decoded video lands in the Toaster raster: the input rectangle (the 4:3 box a source is
// fitted into) and the recording rectangle (spec 5.6 geometry).
#include "vt/raster.h"

extern "C" {
#include <libavutil/rational.h>
}

namespace vt::media {

struct MediaGeometry {
    vt::Rect inputRect{32, 1, 704, 480};
    AVRational pixelAspect{10, 11}; // of the raster pixels, so inputRect is a 4:3 box
    vt::Rect recordRect{24, 1, 720, 480};
};

// The largest rectangle of the source's display aspect that fits inside g.inputRect, centred.
// A sample aspect with a zero or negative term counts as 1:1.
vt::Rect fitRect(int srcW, int srcH, AVRational srcSar, const MediaGeometry& g);

} // namespace vt::media
