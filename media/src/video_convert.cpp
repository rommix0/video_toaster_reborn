#include "vt/media/video_convert.h"

#include <algorithm>
#include <cstdint>
#include <string>

#include "vt/log.h"

extern "C" {
#include <libavutil/pixdesc.h>
}

namespace vt::media {
namespace {

constexpr Rgba8 kBlack{0, 0, 0, 255};

bool isRgbFormat(AVPixelFormat f) {
    const AVPixFmtDescriptor* d = av_pix_fmt_desc_get(f);
    return d && (d->flags & AV_PIX_FMT_FLAG_RGB);
}

bool isFullRangeFormat(AVPixelFormat f) {
    // The deprecated yuvj formats are full range by definition.
    if (f == AV_PIX_FMT_YUVJ420P || f == AV_PIX_FMT_YUVJ422P || f == AV_PIX_FMT_YUVJ444P ||
        f == AV_PIX_FMT_YUVJ440P)
        return true;
    return isRgbFormat(f);
}

AVColorSpace resolveSpace(const AVFrame& f) {
    if (isRgbFormat(AVPixelFormat(f.format))) return AVCOL_SPC_RGB;
    if (f.colorspace != AVCOL_SPC_UNSPECIFIED && f.colorspace != AVCOL_SPC_RESERVED &&
        f.colorspace != AVCOL_SPC_RGB && sws_test_colorspace(f.colorspace, 0))
        return f.colorspace;
    return f.height <= 576 ? AVCOL_SPC_SMPTE170M : AVCOL_SPC_BT709;
}

AVColorRange resolveRange(const AVFrame& f) {
    if (f.color_range == AVCOL_RANGE_MPEG || f.color_range == AVCOL_RANGE_JPEG) return f.color_range;
    return isFullRangeFormat(AVPixelFormat(f.format)) ? AVCOL_RANGE_JPEG : AVCOL_RANGE_MPEG;
}

// The part of r inside the raster (empty when none of it is).
vt::Rect clampToRaster(vt::Rect r) {
    const int x0 = std::clamp(r.x, 0, kRasterW), y0 = std::clamp(r.y, 0, kRasterH);
    const int x1 = std::clamp(r.x + r.w, x0, kRasterW), y1 = std::clamp(r.y + r.h, y0, kRasterH);
    return {x0, y0, x1 - x0, y1 - y0};
}

// Blacks out everything of the raster outside r (rows above and below it, columns beside it).
void fillMargins(vt::Frame& out, const vt::Rect& r) {
    Rgba8* px = out.px.data();
    if (r.w <= 0 || r.h <= 0) {
        std::fill(px, px + out.px.size(), kBlack);
        return;
    }
    std::fill(px, px + size_t(r.y) * kRasterW, kBlack);
    for (int y = r.y; y < r.y + r.h; ++y) {
        Rgba8* row = px + size_t(y) * kRasterW;
        std::fill(row, row + r.x, kBlack);
        std::fill(row + r.x + r.w, row + kRasterW, kBlack);
    }
    std::fill(px + size_t(r.y + r.h) * kRasterW, px + out.px.size(), kBlack);
}

void blackRect(vt::Frame& out, const vt::Rect& r) {
    for (int y = r.y; y < r.y + r.h; ++y) {
        Rgba8* row = out.px.data() + size_t(y) * kRasterW;
        std::fill(row + r.x, row + r.x + r.w, kBlack);
    }
}

} // namespace

VideoConverter::VideoConverter(MediaGeometry g)
    : geo_(g), srcView_(av_frame_alloc()), dstView_(av_frame_alloc()) {}

void VideoConverter::convert(const AVFrame& src, vt::Frame& out) {
    const vt::Rect rect = clampToRaster(fitRect(src.width, src.height, src.sample_aspect_ratio, geo_));
    fillMargins(out, rect);

    const AVColorSpace space = resolveSpace(src);
    const AVColorRange range = resolveRange(src);
    const Key key{src.format, src.width, src.height, src.sample_aspect_ratio.num,
                  src.sample_aspect_ratio.den, int(space), int(range), rect};
    if (key != key_ || !ctx_) {
        key_ = key;
        failed_ = false;
        ctx_.reset(sws_alloc_context());
        if (!ctx_ || !srcView_ || !dstView_) failed_ = true;
        const AVPixFmtDescriptor* d = av_pix_fmt_desc_get(AVPixelFormat(src.format));
        hasAlpha_ = d && (d->flags & AV_PIX_FMT_FLAG_ALPHA);
        // Same size without chroma subsampling (RGB, 4:4:4): only colour changes, so no
        // resampling filter is wanted. Subsampled chroma still has to be upsampled smoothly.
        const bool sameSize = rect.w == src.width && rect.h == src.height;
        const bool fullChroma = d && d->log2_chroma_w == 0 && d->log2_chroma_h == 0;
        if (ctx_) {
            ctx_->flags = sameSize && fullChroma ? SWS_POINT
                                                 : (SWS_BICUBIC | SWS_ACCURATE_RND | SWS_FULL_CHR_H_INT);
            // Slice threads, one per core (output rows split between them; the result is the
            // same): a 4K desktop scaled into the raster takes 24 ms on one thread, 6.4 ms here.
            ctx_->threads = 0;
        }
    }
    if (rect.w <= 0 || rect.h <= 0) return;
    if (failed_) { blackRect(out, rect); return; }

    // Header-only views: the scaler reads the source planes in place and writes straight into
    // `out` at the rectangle, so there is no scratch copy.
    AVFrame* s = srcView_.get();
    s->format = src.format;
    s->width = src.width;
    s->height = src.height;
    s->colorspace = space;
    s->color_range = range;
    s->chroma_location = src.chroma_location;
    for (int i = 0; i < AV_NUM_DATA_POINTERS; ++i) {
        s->data[i] = src.data[i];
        s->linesize[i] = src.linesize[i];
    }
    s->extended_data = s->data;
    AVFrame* d = dstView_.get();
    d->format = AV_PIX_FMT_RGBA;
    d->width = rect.w;
    d->height = rect.h;
    d->colorspace = AVCOL_SPC_RGB;
    d->color_range = AVCOL_RANGE_JPEG;
    d->data[0] = reinterpret_cast<uint8_t*>(out.px.data() + size_t(rect.y) * kRasterW + size_t(rect.x));
    d->linesize[0] = kRasterW * int(sizeof(Rgba8));
    d->extended_data = d->data;

    const int rc = sws_scale_frame(ctx_.get(), d, s);
    if (rc < 0) {
        failed_ = true;
        logWarning("video conversion failed: " + averr(rc));
        blackRect(out, rect);
        return;
    }
    if (hasAlpha_) {
        for (int y = rect.y; y < rect.y + rect.h; ++y) {
            Rgba8* row = out.px.data() + size_t(y) * kRasterW;
            for (int x = rect.x; x < rect.x + rect.w; ++x) row[x].a = 255;
        }
    }
}

} // namespace vt::media
