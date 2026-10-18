#include "effect_detail.h"

namespace vt {

Effect::~Effect() = default;

namespace {

class Cut final : public Effect {
public:
    std::string name() const override { return "Cut"; }
    Timing timing() const override {
        Timing t;  // GUESS: instantaneous, one frame at every speed
        t.durationFrames = {1, 1, 1, 1};
        t.defaultSpeed = Speed::Fast;
        return t;
    }
    int frameCount() const override { return 1; }
    void render(int tbar, const Frame& a, const Frame& b, Frame& out, const RenderCtx&) const override {
        out.px = detail::clampTBar(tbar) < 256 ? a.px : b.px;
    }
};

class Dissolve final : public Effect {
public:
    std::string name() const override { return "Dissolve"; }
    // GUESS: the fade lengths are not in any crouton; these are the ILBM defaults
    // (90/60/30 frames, Variable 60).
    Timing timing() const override {
        Timing t;
        t.durationFrames = {90, 60, 30, 60};
        t.defaultSpeed = Speed::Medium;
        return t;
    }
    int frameCount() const override { return 1; }
    void render(int tbar, const Frame& a, const Frame& b, Frame& out, const RenderCtx&) const override {
        const int t = detail::clampTBar(tbar);
        auto mix = [t](int x, int y) { return uint8_t((x * (511 - t) + y * t + 255) / 511); };
        const size_t n = out.px.size();
        for (size_t i = 0; i < n; ++i) {
            const Rgba8 p = a.px[i], q = b.px[i];
            out.px[i] = {mix(p.r, q.r), mix(p.g, q.g), mix(p.b, q.b), mix(p.a, q.a)};
        }
    }
};

}  // namespace

std::unique_ptr<Effect> makeCut() { return std::make_unique<Cut>(); }
std::unique_ptr<Effect> makeDissolve() { return std::make_unique<Dissolve>(); }

namespace detail {

std::optional<ControlMap> controlMap(int w, int h, std::string* why) {
    auto fail = [&](std::string m) -> std::optional<ControlMap> {
        if (why) *why = std::move(m);
        return std::nullopt;
    };
    if (w != kRasterW && w != kRasterW / 2)
        return fail("control image width " + std::to_string(w) + " is not 768 or 384");
    ControlMap m;
    m.w = w;
    m.h = h;
    if (h >= 1 && h <= 241) m.halfLines = true;
    else if (h == 480 || h == 482) m.halfLines = false;
    else return fail("control image height " + std::to_string(h) + " is not <= 241, 480 or 482");
    return m;
}

}  // namespace detail
}  // namespace vt
