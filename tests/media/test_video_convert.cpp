#include "catch_amalgamated.hpp"
#include "vt/media/video_convert.h"

#include <cstdlib>
#include <cstring>

using namespace vt::media;

namespace {

FramePtr makeFrame(AVPixelFormat fmt, int w, int h) {
    FramePtr f(av_frame_alloc());
    f->format = fmt;
    f->width = w;
    f->height = h;
    REQUIRE(av_frame_get_buffer(f.get(), 0) >= 0);
    return f;
}

void fillPlane(AVFrame& f, int plane, int w, int h, uint8_t v) {
    for (int y = 0; y < h; ++y) std::memset(f.data[plane] + size_t(y) * f.linesize[plane], v, size_t(w));
}

FramePtr solidYuv420(int w, int h, uint8_t y, uint8_t u, uint8_t v) {
    FramePtr f = makeFrame(AV_PIX_FMT_YUV420P, w, h);
    fillPlane(*f, 0, w, h, y);
    fillPlane(*f, 1, w / 2, h / 2, u);
    fillPlane(*f, 2, w / 2, h / 2, v);
    return f;
}

FramePtr solidBgr0(int w, int h, uint8_t v) {
    FramePtr f = makeFrame(AV_PIX_FMT_BGR0, w, h);
    fillPlane(*f, 0, w * 4, h, v);
    return f;
}

vt::Rgba8 centre(const vt::Frame& out) { return out.at(vt::kRasterW / 2, vt::kRasterH / 2); }

bool near(vt::Rgba8 p, int r, int g, int b, int tol) {
    return std::abs(p.r - r) <= tol && std::abs(p.g - g) <= tol && std::abs(p.b - b) <= tol && p.a == 255;
}

vt::Rgba8 convertSolid(int y, int u, int v, int w = 640, int h = 480, AVColorSpace cs = AVCOL_SPC_UNSPECIFIED) {
    FramePtr f = solidYuv420(w, h, uint8_t(y), uint8_t(u), uint8_t(v));
    f->colorspace = cs;
    VideoConverter c;
    vt::Frame out;
    c.convert(*f, out);
    return centre(out);
}

bool isBlack(vt::Rgba8 p) { return p == vt::Rgba8{0, 0, 0, 255}; }
bool isWhite(vt::Rgba8 p) { return p == vt::Rgba8{255, 255, 255, 255}; }

} // namespace

TEST_CASE("limited-range BT.601 levels become full-range RGB") {
    CHECK(near(convertSolid(235, 128, 128), 255, 255, 255, 1));
    CHECK(near(convertSolid(16, 128, 128), 0, 0, 0, 1));
    CHECK(near(convertSolid(81, 90, 240), 255, 0, 0, 3));
}

TEST_CASE("the matrix follows the frame, else the height") {
    const auto sd601 = convertSolid(120, 100, 150, 640, 480, AVCOL_SPC_SMPTE170M);
    const auto sd709 = convertSolid(120, 100, 150, 640, 480, AVCOL_SPC_BT709);
    const auto hd601 = convertSolid(120, 100, 150, 1280, 720, AVCOL_SPC_SMPTE170M);
    const auto hd709 = convertSolid(120, 100, 150, 1280, 720, AVCOL_SPC_BT709);
    REQUIRE_FALSE(sd601 == sd709); // the two matrices really differ for this colour
    CHECK(convertSolid(120, 100, 150, 640, 480) == sd601);
    CHECK(convertSolid(120, 100, 150, 1280, 720) == hd709);
    CHECK(hd601 != hd709);
}

TEST_CASE("full-range YUV frames are not stretched") {
    FramePtr f = solidYuv420(640, 480, 255, 128, 128);
    f->color_range = AVCOL_RANGE_JPEG;
    VideoConverter c;
    vt::Frame out;
    c.convert(*f, out);
    CHECK(near(centre(out), 255, 255, 255, 1));
    fillPlane(*f, 0, 640, 480, 16);
    c.convert(*f, out);
    CHECK(near(centre(out), 16, 16, 16, 1)); // not pulled down to 0
}

TEST_CASE("704x480 SAR 10:11 RGB is copied pixel for pixel") {
    FramePtr f = makeFrame(AV_PIX_FMT_BGR0, 704, 480);
    f->sample_aspect_ratio = {10, 11};
    for (int y = 0; y < 480; ++y)
        for (int x = 0; x < 704; ++x) {
            uint8_t* p = f->data[0] + size_t(y) * f->linesize[0] + size_t(x) * 4;
            p[0] = uint8_t((x >> 8) * 64 + (y >> 8) * 16); // b
            p[1] = uint8_t(y & 255);                       // g
            p[2] = uint8_t(x & 255);                       // r
            p[3] = 0;
        }
    VideoConverter c;
    vt::Frame out;
    c.convert(*f, out);
    int bad = 0;
    for (int y = 0; y < 480; ++y)
        for (int x = 0; x < 704; ++x) {
            const vt::Rgba8 want{uint8_t(x & 255), uint8_t(y & 255), uint8_t((x >> 8) * 64 + (y >> 8) * 16), 255};
            if (!(out.at(32 + x, 1 + y) == want)) ++bad;
        }
    CHECK(bad == 0);
}

TEST_CASE("16:9 is letterboxed, portrait pillarboxed, margins black") {
    VideoConverter c;
    vt::Frame out;
    {
        FramePtr f = solidBgr0(1920, 1080, 255);
        c.convert(*f, out);
        for (int y = 0; y < vt::kRasterH; ++y)
            for (int x = 0; x < vt::kRasterW; ++x) {
                const bool inside = y >= 61 && y <= 420 && x >= 32 && x <= 735;
                INFO("x " << x << " y " << y);
                REQUIRE((inside ? isWhite(out.at(x, y)) : isBlack(out.at(x, y))));
            }
    }
    {
        FramePtr f = solidBgr0(1080, 1920, 255);
        c.convert(*f, out);
        for (int y = 0; y < vt::kRasterH; ++y)
            for (int x = 0; x < vt::kRasterW; ++x) {
                const bool inside = y >= 1 && y <= 480 && x >= 235 && x <= 531;
                INFO("x " << x << " y " << y);
                REQUIRE((inside ? isWhite(out.at(x, y)) : isBlack(out.at(x, y))));
            }
    }
}

TEST_CASE("margins are black even when out arrives dirty") {
    VideoConverter c;
    vt::Frame out;
    for (auto& p : out.px) p = {9, 8, 7, 6};
    FramePtr f = solidBgr0(1920, 1080, 255);
    c.convert(*f, out);
    CHECK(isBlack(out.at(0, 0)));
    CHECK(isBlack(out.at(10, 100)));
    CHECK(isBlack(out.at(760, 300)));
    CHECK(isBlack(out.at(400, 481)));
    CHECK(isBlack(out.at(400, 30)));
    CHECK(isWhite(out.at(400, 240)));
}

TEST_CASE("source alpha does not reach the raster") {
    FramePtr f = makeFrame(AV_PIX_FMT_RGBA, 704, 480);
    for (int y = 0; y < 480; ++y) std::memset(f->data[0] + size_t(y) * f->linesize[0], 0x40, 704 * 4);
    f->sample_aspect_ratio = {10, 11};
    VideoConverter c;
    vt::Frame out;
    c.convert(*f, out);
    CHECK(out.at(400, 240) == vt::Rgba8{0x40, 0x40, 0x40, 255});
}

TEST_CASE("steady state does not reallocate") {
    VideoConverter c;
    vt::Frame out;
    const vt::Rgba8* buf = out.px.data();
    CHECK(c.contextForTest() == nullptr);
    FramePtr f = solidYuv420(640, 480, 120, 100, 150);
    c.convert(*f, out);
    const SwsContext* ctx = c.contextForTest();
    REQUIRE(ctx != nullptr);
    for (int i = 0; i < 10; ++i) {
        fillPlane(*f, 0, 640, 480, uint8_t(100 + i));
        c.convert(*f, out);
        CHECK(out.px.data() == buf);
        CHECK(c.contextForTest() == ctx);
    }
    // A different source format gets a different context.
    FramePtr g = solidBgr0(640, 480, 10);
    c.convert(*g, out);
    CHECK(c.contextForTest() != nullptr);
}

TEST_CASE("same-size sources skip the scaling filter only without chroma subsampling") {
    // Ruling 7: nearest-neighbour would upsample 4:2:0 chroma into blocky colour edges.
    auto flagsFor = [](AVPixelFormat fmt) {
        FramePtr f = makeFrame(fmt, 704, 480);
        f->sample_aspect_ratio = {10, 11}; // fills inputRect at its own size
        VideoConverter c;
        vt::Frame out;
        c.convert(*f, out);
        REQUIRE(c.contextForTest() != nullptr);
        return c.contextForTest()->flags;
    };
    CHECK(flagsFor(AV_PIX_FMT_BGR0) == SWS_POINT);
    CHECK(flagsFor(AV_PIX_FMT_YUV444P) == SWS_POINT);
    const unsigned bicubic = SWS_BICUBIC | SWS_ACCURATE_RND | SWS_FULL_CHR_H_INT;
    CHECK(flagsFor(AV_PIX_FMT_YUV420P) == bicubic);
    CHECK(flagsFor(AV_PIX_FMT_NV12) == bicubic);
}
