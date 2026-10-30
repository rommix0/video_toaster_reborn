// The 14 Bar ILBM croutons (4.2/Effects/4000FX/Border): one still picture that the Switcher shows
// every field at an offset (x, y) from channel 29 of the crouton's header-less program, its palette
// indices routed by tag 0x1B. Sections cited as "BARS.md N" are in tools/re/BARS.md; this is the
// default ("one sweep") model of tools/re/bars_ref.py, which tests/golden/bars.jsonl pins.
#include <algorithm>
#include <array>
#include <exception>
#include <string>
#include <vector>

#include "effect_detail.h"
#include "vt/algo.h"

namespace vt::detail {
namespace {

constexpr uint32_t kTagProgram = 0x1000001C;
constexpr uint32_t kTagSourceWords = 0x1000001B;
constexpr uint32_t kTagWholeDepth = 0x8000001E;  // $2b bit 7: all planes, no plane-half switch
constexpr uint32_t kTagLace = 0x80000041;        // $2b bit 0: interlaced picture, y base 0xf1
constexpr int kChannel = 29;

// BARS.md 5.1: the T-bar sets $30e = pos << 7 (0 gives 1, 511 gives 0xffff); Auto step k of N
// sets min(k * floor(65536 / N), 0xffff). t = $30e (16.16). The original's Auto renders N + 1
// fields ending at 0xffff; the engine's last Auto step is t = N * floor(65536 / N) (65520 for
// N = 60), then the take ends on B.
uint32_t barT(int tbar, int autoStep, int n) {
    if (autoStep >= 1) return std::min(uint32_t(autoStep) * (0x10000u / uint32_t(std::max(n, 1))), 0xffffu);
    tbar = clampTBar(tbar);
    return tbar >= 511 ? 0xffff : tbar == 0 ? 1 : uint32_t(tbar) << 7;
}

class BarEffect final : public CroutonEffect {
public:
    struct Entry {
        MixSrc src = MixSrc::Colour;
        Rgba8 colour{0, 0, 0, 255};
        bool generator = false;  // a plain S = 9 route: the colour generator (none ships)
    };

    BarEffect(CroutonBasics b, std::vector<uint8_t> program, std::vector<IndexImage> pics, std::array<Entry, 256> table,
              bool lace)
        : CroutonEffect(std::move(b)), program_(std::move(program)), pics_(std::move(pics)), table_(table),
          lace_(lace) {}

    int frameCount() const override { return 1; }

    // BARS.md 5: screen pixel (sx, sy) is field f = sy & 1, field line k = sy >> 1, field row
    // r = k - y. Outside 0 <= r < rows the planes are off: palette index 0. Else picture row
    // 2r + f (r without tag 0x41), column floor((2 sx - x) / 2) clamped to the picture.
    //   GUESS (BARS.md 6): x counts half hires pixels (the code reads lores pixels, 4x as much);
    //   GUESS: columns outside the picture repeat its edge column;
    //   GUESS: index 0 shows what its route says (A / B on the tall bars, CMAP 0 on the wide ones,
    //   whose unrouted 0x10000000 keeps the palette's own bits).
    // Colour: tag 0x1B S 1 -> A, S 3 -> B, anything else (the 0x9003 art words) the CMAP colour.
    void render(int tbar, const Frame& a, const Frame& b, Frame& out, const RenderCtx& ctx) const override {
        const Timing& tm = basics().timing;
        const int n = ctx.autoStep >= 1 && ctx.autoSteps > 0
                          ? ctx.autoSteps
                          : tm.durationTicks[size_t(ctx.speed.value_or(tm.defaultSpeed))];
        const auto w = *algoChannelWords(program_, kChannel, barT(tbar, ctx.autoStep, n));  // probed at load
        int x = int16_t((0x17e - w[0]) * 2 + w[2]);
        const int y = int16_t((lace_ ? 0xf1 : 0x78) - w[1] + w[3]);
        // BARS.md 5.3 (0xe7c0a): without tag 0x1E half the planes show, the upper half (the same
        // picture 32 px to the right) with x - 64 when bit 6 of x - 4 is clear.
        const IndexImage* img = &pics_[0];
        if (pics_.size() == 2 && !((x - 4) & 0x40)) {
            img = &pics_[1];
            x -= 64;
        }
        const int rows = lace_ ? img->h / 2 : img->h;  // field rows ($16)
        const int shift = (-x) >> 1;                   // floor((2 sx - x) / 2) = sx + floor(-x / 2)
        for (int sy = 0; sy < kRasterH; ++sy) {
            const int r = (sy >> 1) - y;
            const uint8_t* src = nullptr;
            if (r >= 0 && r < rows)
                src = &img->px[size_t(std::min(lace_ ? 2 * r + (sy & 1) : r, img->h - 1)) * size_t(img->w)];
            Rgba8* dst = &out.at(0, sy);
            const Rgba8* pa = &a.at(0, sy);
            const Rgba8* pb = &b.at(0, sy);
            const Rgba8* pg = ctx.generator ? &ctx.generator->at(0, sy) : nullptr;
            for (int sx = 0; sx < kRasterW; ++sx) {
                const Entry& e = table_[src ? src[std::clamp(sx + shift, 0, img->w - 1)] : 0];
                dst[sx] = e.src == MixSrc::A   ? pa[sx]
                          : e.src == MixSrc::B ? pb[sx]
                          : e.generator        ? (pg ? pg[sx] : ctx.background)
                                               : e.colour;
            }
        }
    }

private:
    std::vector<uint8_t> program_;
    std::vector<IndexImage> pics_;  // tag 0x1E: all planes; else the lower and the upper half
    std::array<Entry, 256> table_;
    bool lace_;
};

}  // namespace

std::unique_ptr<Effect> makeBarEffect(CroutonBasics b, const Crouton& c, const Form& form, std::string* why) {
    auto fail = [&](std::string reason) -> std::unique_ptr<Effect> {
        if (why) *why = std::move(reason);
        return nullptr;
    };
    const auto& program = c.blobs.at(kTagProgram);
    try {
        // A probe of every t the effect can be asked for (the T-bar, Auto at each speed), so a
        // program that cannot run is refused here and render never throws (as ALGO's Ruling 5).
        if (!algoChannelWords(program, kChannel, 1)) return fail("Bar program without channel 29");
        for (int pos = 0; pos <= 511; ++pos) algoChannelWords(program, kChannel, barT(pos, -1, 0));
        for (int n : b.timing.durationTicks)
            for (int k = 1; k <= std::min(n, 0x10000); ++k) algoChannelWords(program, kChannel, barT(0, k, n));
    } catch (const std::exception& ex) {
        return fail(std::string("Bar program not supported: ") + ex.what());
    }

    auto [bm, planes] = readIlbmPlanes(c.bytes, form);
    const bool split = !c.tag(kTagWholeDepth);
    const int shown = split ? planes.count / 2 : planes.count;
    std::vector<IndexImage> pics;
    for (int p0 = 0; p0 < (split ? 2 : 1); ++p0) {
        Planes part{shown, planes.h, planes.rowbytes, {}};
        const auto* from = planes.plane(p0 * shown);
        part.bytes.assign(from, from + size_t(shown) * size_t(planes.h) * size_t(planes.rowbytes));
        pics.push_back(planesToIndex(part, bm.w));  // throws past 8 planes
    }

    const auto tag = c.blobs.find(kTagSourceWords);
    const auto routes = sourceRoutes(tag != c.blobs.end() ? tag->second : std::vector<uint8_t>{}, 1 << shown);
    // Without them the take would be palette art alone, never A or B
    if (std::none_of(routes.begin(), routes.end(),
                     [](PaletteRoute r) { return r == PaletteRoute::A || r == PaletteRoute::B; }))
        return fail("Bar tag 0x1B has no A/B routes");
    std::array<BarEffect::Entry, 256> table{};
    for (size_t i = 0; i < table.size(); ++i) {
        if (!bm.cmap.empty()) {
            const Rgb& p = bm.cmap[i % bm.cmap.size()];  // bars_ref: cm[i % len(cm)]
            table[i].colour = {p.r, p.g, p.b, 255};
        }
        switch (i < routes.size() ? routes[i] : PaletteRoute::Palette) {
            case PaletteRoute::A: table[i].src = MixSrc::A; break;
            case PaletteRoute::B: table[i].src = MixSrc::B; break;
            case PaletteRoute::Generator: table[i].generator = true; break;
            case PaletteRoute::Palette: break;
        }
    }
    return std::make_unique<BarEffect>(std::move(b), program, std::move(pics), table, c.tag(kTagLace).has_value());
}

}  // namespace vt::detail
