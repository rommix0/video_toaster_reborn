#pragma once
// Decoded video into the engine raster: any AVFrame is colour-converted to RGBA, scaled to the
// rectangle fitRect gives it, and placed there; the rest of the raster is opaque black.
#include "vt/media/ffmpeg.h"
#include "vt/media/geometry.h"
#include "vt/raster.h"

namespace vt::media {

class VideoConverter {
public:
    explicit VideoConverter(MediaGeometry = {});

    // Writes all of `out`: the picture in the fitted rectangle, opaque black elsewhere. The
    // context and scratch are reused while the source format, size, SAR and colour stay the
    // same, so steady state does not allocate. A source swscale cannot convert gives a black
    // raster (and one warning until the source changes).
    // Colour matrix: the frame's colorspace, else BT.601 up to 576 lines and BT.709 above.
    // Range: the frame's color_range, else limited for YUV and full for RGB.
    void convert(const AVFrame& src, vt::Frame& out);

    // The scaling context in use (null before the first frame), to check that it is reused.
    const SwsContext* contextForTest() const { return ctx_.get(); }

private:
    // What the scaler setup depends on; a change rebuilds the context.
    struct Key {
        int format = -1, w = 0, h = 0, sarNum = 0, sarDen = 0, space = 0, range = 0;
        vt::Rect rect{};
        bool operator==(const Key&) const = default;
    };

    MediaGeometry geo_;
    Key key_;
    SwsPtr ctx_;
    FramePtr srcView_, dstView_; // frame headers only: they borrow the caller's and `out`'s memory
    bool hasAlpha_ = false;      // the source carries alpha, which the output must not
    bool failed_ = false;        // this source cannot be converted; its warning was given
};

} // namespace vt::media
