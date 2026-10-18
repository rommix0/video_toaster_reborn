// Gradient (luma) wipes: an ILBM whose gray values say when each pixel switches from A to B.
#include <algorithm>
#include <array>

#include "effect_detail.h"

namespace vt {

// toaster_fx_convert.gray_lut
std::vector<uint8_t> grayLut(const Bitmap& bm) {
    const size_t n = size_t(1) << bm.planes;
    const auto& cm = bm.cmap;
    if (cm.size() >= n) {
        bool ramp = true;
        int mx = 0;
        for (size_t i = 0; i < n; ++i) {
            if (cm[i].r != cm[i].g || cm[i].g != cm[i].b) ramp = false;
            mx = std::max(mx, int(cm[i].r));
        }
        if (ramp && mx > 0x40) {
            std::vector<uint8_t> g(n);
            for (size_t i = 0; i < n; ++i) g[i] = cm[i].r;
            return g;
        }
    }
    std::vector<uint8_t> g(n);
    for (size_t i = 0; i < n; ++i) g[i] = uint8_t(n > 1 ? i * 255 / (n - 1) : 0);
    return g;
}

// Controller Ruling 12. With u = (gray + 1) * 511 - P, where P is the T-bar's threshold:
//   hard edge (softness 0): P = (511 - tbar) * 256, B iff u > 0, i.e.
//     (gray + 1) * 511 > (511 - tbar) * 256.
//     tbar 0 shows no B (even gray 255 gives u = 0), tbar 511 shows all B (u >= 511), bright grays
//     are revealed first and a revealed pixel stays revealed.
//   softness s > 0: the edge is a linear ramp over s gray levels (s * 511 units of u):
//     P = (511 - tbar) * (256 + s) - 511 * s,  level = clamp(255 * u / (s * 511), 0, 255).
//     P(0) = 511 * 256, so tbar 0 is all A; P(511) = -511 * s, so tbar 511 is all B (u >= 511 * (1 + s));
//     s -> 0 gives the hard edge above exactly.
uint8_t gradientLevel(uint8_t gray, int tbar, int softness) {
    const int t = tbar < 0 ? 0 : (tbar > 511 ? 511 : tbar);
    const int s = softness < 0 ? 0 : (softness > 255 ? 255 : softness);
    const int u = (int(gray) + 1) * 511 - ((511 - t) * (256 + s) - 511 * s);
    if (u <= 0) return 0;
    if (s == 0) return 255;
    return uint8_t(std::min(255, 255 * u / (s * 511)));
}

namespace detail {
namespace {

class GradientEffect final : public Effect {
public:
    GradientEffect(std::string name, Timing t, std::optional<Sound> snd, ControlMap map, IndexImage img,
                   std::vector<uint8_t> gray)
        : name_(std::move(name)), timing_(t), sound_(std::move(snd)), map_(map), img_(std::move(img)),
          gray_(std::move(gray)) {
        gray_.resize(256, 0);  // indices past the palette cannot occur; keep lookups in range
    }

    std::string name() const override { return name_; }
    Timing timing() const override { return timing_; }
    int frameCount() const override { return 1; }
    const Sound* sound() const override { return sound_ ? &*sound_ : nullptr; }

    // Per pixel, B's share is gradientLevel(gray, tbar, softness); the picture is
    // A + (B - A) * level / 255 per channel (mix255). Raster lines with no control line are black.
    void render(int tbar, const Frame& a, const Frame& b, Frame& out, const RenderCtx& ctx) const override {
        const int t = clampTBar(tbar);
        const int s = std::clamp(ctx.guesses.gradientSoftness, 0, 255);
        std::array<uint8_t, 256> level{};  // per palette index
        for (int i = 0; i < 256; ++i) level[size_t(i)] = gradientLevel(gray_[size_t(i)], t, s);
        for (int y = 0; y < kRasterH; ++y) {
            Rgba8* dst = &out.at(0, y);
            const int line = map_.line(y);
            if (line < 0) {
                std::fill(dst, dst + kRasterW, kBlack);
                continue;
            }
            const uint8_t* src = &img_.px[size_t(line) * size_t(img_.w)];
            const Rgba8* pa = &a.at(0, y);
            const Rgba8* pb = &b.at(0, y);
            for (int x = 0; x < kRasterW; ++x) dst[x] = mixPixel(pa[x], pb[x], level[src[map_.x(x)]]);
        }
    }

private:
    std::string name_;
    Timing timing_;
    std::optional<Sound> sound_;
    ControlMap map_;
    IndexImage img_;
    std::vector<uint8_t> gray_;
};

}  // namespace

const char* gradientRefusal(const Bitmap& bm) {
    // Bar pictures are taller than the raster; the Switcher code moves them (Ruling 11).
    if (bm.h > kRasterH) return "bar picture moved by Switcher code (not reverse-engineered yet)";
    if (bm.planes > 8) return "deep-colour bar picture moved by Switcher code (like ALGO)";
    if (bm.camg & 0x800) return "not a grayscale matte (HAM image)";
    return nullptr;
}

std::unique_ptr<Effect> makeGradient(std::string name, Timing timing, std::optional<Sound> snd, Bitmap bm,
                                     IndexImage img, std::string* why) {
    if (const char* m = gradientRefusal(bm)) {
        if (why) *why = m;
        return nullptr;
    }
    auto map = controlMap(bm.w, bm.h, why);
    if (!map) return nullptr;
    auto gray = grayLut(bm);
    return std::make_unique<GradientEffect>(std::move(name), timing, std::move(snd), *map, std::move(img),
                                            std::move(gray));
}

}  // namespace detail
}  // namespace vt
