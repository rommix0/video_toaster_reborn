#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

#include "catch_amalgamated.hpp"
#include "golden.h"
#include "vt/anim.h"
#include "vt/background.h"
#include "vt/crouton.h"
#include "vt/dve.h"
#include "vt/effect.h"
#include "vt/ilbm.h"
#include "vt/log.h"
#include "vt/mix.h"
#include "vt/sound.h"
#include "vt/timing.h"

namespace {

constexpr std::array<char, 4> kAnim{'A', 'N', 'I', 'M'};
constexpr std::array<char, 4> kIlbm{'I', 'L', 'B', 'M'};
constexpr uint32_t kTagMode = 0x80000022;
constexpr uint32_t kTagSourceWords = 0x1000001B;
const vt::Rgba8 kA{10, 20, 30, 255}, kB{200, 150, 100, 255};
const vt::Rgba8 kWhite{255, 255, 255, 255}, kBlack{0, 0, 0, 255};

int64_t num(const golden::Record& r, const char* key) { return std::get<int64_t>(r.at(key).v); }
const std::string& str(const golden::Record& r, const char* key) { return std::get<std::string>(r.at(key).v); }
const std::vector<int64_t>& arr(const golden::Record& r, const char* key) {
    return std::get<std::vector<int64_t>>(r.at(key).v);
}

bool same(vt::Rgba8 a, vt::Rgba8 b) { return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a; }

vt::Frame solid(vt::Rgba8 c) {
    vt::Frame f;
    std::fill(f.px.begin(), f.px.end(), c);
    return f;
}

// Every pixel different, so a wrong source pixel shows up.
vt::Frame pattern(unsigned seed) {
    vt::Frame f;
    for (size_t i = 0; i < f.px.size(); ++i) {
        const unsigned v = unsigned(i) * 2654435761u + seed * 40503u;
        f.px[i] = {uint8_t(v >> 8), uint8_t(v >> 16), uint8_t(v >> 24), uint8_t(255 - (v & 0x3f))};
    }
    return f;
}

vt::RenderCtx ctxWith(vt::Rgba8 bg = kWhite) {
    vt::RenderCtx c;
    c.background = bg;
    return c;
}

vt::Frame renderAt(const vt::Effect& e, int tbar, const vt::Frame& a, const vt::Frame& b, vt::RenderCtx c = ctxWith()) {
    vt::Frame out;
    e.render(tbar, a, b, out, c);
    return out;
}

// First tbar that animFrameForTBar maps to frame f.
int tbarForFrame(int f, int n) {
    for (int t = 0; t < 512; ++t)
        if (vt::animFrameForTBar(t, n) == f) return t;
    FAIL("no tbar for frame " << f << " of " << n);
    return 0;
}

// The integer mix the engine documents: l = 0 -> a, l = 255 -> b, else round half up.
uint8_t lerp255(int a, int b, int l) { return uint8_t((a * (255 - l) + b * l + 127) / 255); }
vt::Rgba8 lerpPx(vt::Rgba8 a, vt::Rgba8 b, int l) {
    return {lerp255(a.r, b.r, l), lerp255(a.g, b.g, l), lerp255(a.b, b.b, l), lerp255(a.a, b.a, l)};
}
// Colour over the A/B mix with the colour's own opacity.
vt::Rgba8 over(vt::Rgba8 m, vt::Rgba8 c) {
    return {lerp255(m.r, c.r, c.a), lerp255(m.g, c.g, c.a), lerp255(m.b, c.b, c.a), m.a};
}

// Control image -> raster, written out independently of the engine (Ruling 11).
struct Ctl {
    int w, h;
    int x(int rx) const { return w == 768 ? rx : rx / 2; }
    int line(int ry) const {
        const int k = h <= 241 ? ry / 2 : ry;
        return k < h ? k : -1;
    }
};

std::optional<vt::Form> firstForm(const vt::Crouton& c, std::array<char, 4> type) {
    for (auto& f : c.forms)
        if (f.type == type) return f;
    return std::nullopt;
}

bool isIcon(const vt::Crouton& c, const vt::Form& f) {
    auto bm = vt::readBitmapHeader(c.bytes, f);
    return bm.w == 80 && bm.h == 50;
}

std::optional<vt::Form> gradientForm(const vt::Crouton& c) {
    for (auto& f : c.forms)
        if (f.type == kIlbm && !isIcon(c, f)) return f;
    return std::nullopt;
}

struct AnimFile {
    vt::Crouton crouton;
    vt::Anim anim;
    vt::AnimPlan plan;
};
AnimFile openAnim(const std::string& source) {
    auto c = vt::loadCrouton(golden::sourcePath(source));
    REQUIRE(c);
    auto f = firstForm(*c, kAnim);
    REQUIRE(f);
    vt::Anim a = vt::readAnim(c->bytes, *f);
    vt::AnimPlan p = vt::planAnim(a, c->tag(kTagMode));
    if (auto it = c->blobs.find(kTagSourceWords); it != c->blobs.end()) p.routes = vt::paletteRoutes(it->second, a.bm.planes);
    return {std::move(*c), std::move(a), std::move(p)};
}

std::unique_ptr<vt::Effect> load(const std::string& source, std::string* why = nullptr) {
    return vt::loadEffect(golden::sourcePath(source), why);
}

// Compare a whole frame with a per-pixel expectation; one REQUIRE with the first mismatch.
template <class F>
void requireFrame(const vt::Frame& out, F expected) {
    int bad = 0, bx = -1, by = -1;
    vt::Rgba8 want{}, got{};
    for (int y = 0; y < vt::kRasterH && bad < 1; ++y)
        for (int x = 0; x < vt::kRasterW; ++x) {
            const vt::Rgba8 e = expected(x, y);
            if (!same(e, out.at(x, y))) {
                ++bad;
                bx = x;
                by = y;
                want = e;
                got = out.at(x, y);
                break;
            }
        }
    INFO("first mismatch at (" << bx << "," << by << ") want " << int(want.r) << "," << int(want.g) << ","
                               << int(want.b) << "," << int(want.a) << " got " << int(got.r) << "," << int(got.g)
                               << "," << int(got.b) << "," << int(got.a));
    REQUIRE(bad == 0);
}

}  // namespace

TEST_CASE("hardware guesses carry the plan's defaults", "[effect]") {
    vt::HardwareGuesses g;
    REQUIRE(g.gradientSoftness == 0);
    REQUIRE(g.dveShadowOpacity == 0.5f);
    REQUIRE(g.dveCellPixel == -1);
    REQUIRE(g.chromaLumaOffset == 17.5f);
    vt::RenderCtx c;
    REQUIRE(c.guesses.gradientSoftness == 0);
}

TEST_CASE("cut shows A below tbar 256 and B from 256", "[effect][cut]") {
    auto cut = vt::makeCut();
    REQUIRE(!cut->usesDvMemory());
    REQUIRE(cut->sound() == nullptr);
    const vt::Frame a = pattern(1), b = pattern(2);
    for (int t : {0, 1, 255}) REQUIRE(renderAt(*cut, t, a, b).px == a.px);
    for (int t : {256, 257, 511}) REQUIRE(renderAt(*cut, t, a, b).px == b.px);
}

TEST_CASE("dissolve is exactly A at 0 and exactly B at 511", "[effect][dissolve]") {
    auto d = vt::makeDissolve();
    const vt::Frame a = pattern(3), b = pattern(4);
    REQUIRE(renderAt(*d, 0, a, b).px == a.px);
    REQUIRE(renderAt(*d, 511, a, b).px == b.px);
    // exact integer mix at other positions
    for (int t : {1, 100, 255, 256, 400, 510}) {
        const vt::Frame o = renderAt(*d, t, a, b);
        requireFrame(o, [&](int x, int y) {
            const vt::Rgba8 p = a.at(x, y), q = b.at(x, y);
            auto m = [t](int u, int v) { return uint8_t((u * (511 - t) + v * t + 255) / 511); };
            return vt::Rgba8{m(p.r, q.r), m(p.g, q.g), m(p.b, q.b), m(p.a, q.a)};
        });
    }
    // monotone between two flat colours
    const vt::Frame sa = solid({0, 0, 0, 255}), sb = solid({255, 255, 255, 255});
    int prev = -1;
    for (int t = 0; t < 512; t += 7) {
        const int v = renderAt(*d, t, sa, sb).at(5, 5).r;
        REQUIRE(v >= prev);
        prev = v;
    }
}

TEST_CASE("gray LUT matches the converter for every gradient ILBM", "[effect][golden]") {
    const auto recs = golden::readLines("ilbm.jsonl");
    REQUIRE(recs.size() == 113);
    for (const auto& r : recs) {
        INFO(str(r, "source"));
        auto c = vt::loadCrouton(golden::sourcePath(str(r, "source")));
        REQUIRE(c);
        auto f = gradientForm(*c);
        REQUIRE(f);
        const auto bm = vt::readBitmapHeader(c->bytes, *f);
        const auto lut = vt::grayLut(bm);
        const auto& want = arr(r, "gray_lut");
        REQUIRE(lut.size() == want.size());
        for (size_t i = 0; i < lut.size(); ++i) REQUIRE(int64_t(lut[i]) == want[i]);
    }
}

TEST_CASE("gray LUT falls back to a linear ramp unless the palette is a gray ramp", "[effect]") {
    vt::Bitmap bm;
    bm.planes = 2;
    REQUIRE(vt::grayLut(bm) == std::vector<uint8_t>{0, 85, 170, 255});  // no palette
    bm.cmap = {{0, 0, 0}, {0x10, 0x10, 0x10}, {0x20, 0x20, 0x20}, {0x40, 0x40, 0x40}};  // max not > 0x40
    REQUIRE(vt::grayLut(bm) == std::vector<uint8_t>{0, 85, 170, 255});
    bm.cmap = {{0, 0, 0}, {0x10, 0x10, 0x10}, {0x20, 0x20, 0x20}, {0x41, 0x41, 0x41}};
    REQUIRE(vt::grayLut(bm) == std::vector<uint8_t>{0, 0x10, 0x20, 0x41});
    bm.cmap = {{0, 0, 0}, {0x10, 0x10, 0x11}, {0x20, 0x20, 0x20}, {0x90, 0x90, 0x90}};  // not r=g=b
    REQUIRE(vt::grayLut(bm) == std::vector<uint8_t>{0, 85, 170, 255});
    bm.cmap = {{0, 0, 0}, {1, 1, 1}};  // shorter than 1 << planes
    REQUIRE(vt::grayLut(bm) == std::vector<uint8_t>{0, 85, 170, 255});
}

namespace {

struct Gradient {
    vt::Bitmap bm;
    vt::IndexImage img;
    std::vector<uint8_t> gray;
};
Gradient openGradient(const std::string& source) {
    auto c = vt::loadCrouton(golden::sourcePath(source));
    REQUIRE(c);
    auto f = gradientForm(*c);
    REQUIRE(f);
    auto [bm, img] = vt::readIlbm(c->bytes, *f);
    auto g = vt::grayLut(bm);
    return {std::move(bm), std::move(img), std::move(g)};
}

// Ends of every gradient wipe, stated without the threshold rule: tbar 0 is exactly A and tbar 511
// exactly B (non-solid frames, so a wrong source-pixel read shows), except raster lines with no
// control line, which are black.
void requireGradientEnds(const vt::Effect& e, const Gradient& g, int softness = 0) {
    const Ctl ctl{g.img.w, g.img.h};
    const vt::Frame a = pattern(5), b = pattern(6);
    vt::RenderCtx c = ctxWith();
    c.guesses.gradientSoftness = softness;
    requireFrame(renderAt(e, 0, a, b, c), [&](int x, int y) { return ctl.line(y) < 0 ? kBlack : a.at(x, y); });
    requireFrame(renderAt(e, 511, a, b, c), [&](int x, int y) { return ctl.line(y) < 0 ? kBlack : b.at(x, y); });
}

// In between: each pixel is the A/B mix at the level gradientLevel (tested on its own) gives for the
// pixel's control gray; with softness 0 that is A or B. Control mapping and source reads are
// checked here, the threshold rule is not restated.
void requireGradient(const vt::Effect& e, const Gradient& g, int tbar, int softness = 0) {
    const Ctl ctl{g.img.w, g.img.h};
    const vt::Frame a = pattern(7), b = pattern(8);
    vt::RenderCtx c = ctxWith();
    c.guesses.gradientSoftness = softness;
    const vt::Frame out = renderAt(e, tbar, a, b, c);
    requireFrame(out, [&](int x, int y) {
        const int line = ctl.line(y);
        if (line < 0) return kBlack;
        const int gray = g.gray[g.img.px[size_t(line) * size_t(g.img.w) + size_t(ctl.x(x))]];
        const int level = vt::gradientLevel(uint8_t(gray), tbar, softness);
        if (softness == 0) REQUIRE((level == 0 || level == 255));
        return lerpPx(a.at(x, y), b.at(x, y), level);
    });
}

}  // namespace

TEST_CASE("gradientLevel: never B at tbar 0, always B at 511, monotone in tbar and gray", "[effect][gradient]") {
    for (int softness : {0, 1, 4, 16, 64}) {
        INFO("softness " << softness);
        for (int gray = 0; gray < 256; ++gray) {
            INFO("gray " << gray);
            REQUIRE(vt::gradientLevel(uint8_t(gray), 0, softness) == 0);
            REQUIRE(vt::gradientLevel(uint8_t(gray), 511, softness) == 255);
            int prev = 0;
            for (int t = 0; t < 512; ++t) {
                const int l = vt::gradientLevel(uint8_t(gray), t, softness);
                if (softness == 0) REQUIRE((l == 0 || l == 255));
                if (l < prev) FAIL("level fell with tbar at gray " << gray << " tbar " << t);
                prev = l;
            }
        }
        for (int t = 0; t < 512; ++t) {
            int prev = 255;  // brighter grays are revealed no later than darker ones
            for (int gray = 255; gray >= 0; --gray) {
                const int l = vt::gradientLevel(uint8_t(gray), t, softness);
                if (l > prev) FAIL("level rose with darker gray at tbar " << t << " gray " << gray);
                prev = l;
            }
        }
    }
}

TEST_CASE("gradientLevel hard edge: B iff (gray + 1) * 511 > (511 - tbar) * 256", "[effect][gradient]") {
    for (int gray = 0; gray < 256; ++gray)
        for (int t = 0; t < 512; ++t)
            REQUIRE((vt::gradientLevel(uint8_t(gray), t, 0) == 255) == ((gray + 1) * 511 > (511 - t) * 256));
    // spot values
    REQUIRE(vt::gradientLevel(255, 0, 0) == 0);
    REQUIRE(vt::gradientLevel(255, 1, 0) == 255);
    REQUIRE(vt::gradientLevel(127, 255, 0) == 0);
    REQUIRE(vt::gradientLevel(127, 256, 0) == 255);
    REQUIRE(vt::gradientLevel(0, 509, 0) == 0);
    REQUIRE(vt::gradientLevel(0, 510, 0) == 255);
    REQUIRE(vt::gradientLevel(0, 511, 0) == 255);
    // out-of-range inputs are clamped
    REQUIRE(vt::gradientLevel(255, -5, 0) == 0);
    REQUIRE(vt::gradientLevel(0, 900, 0) == 255);
}

TEST_CASE("gradientLevel softness ramps through partial levels and agrees with the hard edge as it shrinks", "[effect][gradient]") {
    // some partial level exists for softness > 0 and none for 0
    for (int softness : {1, 4, 16, 64}) {
        bool partial = false;
        for (int gray = 0; gray < 256 && !partial; ++gray)
            for (int t = 0; t < 512; ++t) {
                const int l = vt::gradientLevel(uint8_t(gray), t, softness);
                if (l > 0 && l < 255) { partial = true; break; }
            }
        REQUIRE(partial);
    }
    // a wider ramp never reveals a pixel earlier than the hard edge does
    for (int softness : {1, 4, 16, 64})
        for (int gray = 0; gray < 256; ++gray)
            for (int t = 0; t < 512; ++t)
                if (vt::gradientLevel(uint8_t(gray), t, softness) == 255)
                    REQUIRE(vt::gradientLevel(uint8_t(gray), t, 0) == 255);
}

TEST_CASE("Wipe Border Diamonds: all A at tbar 0, all B at 511", "[effect][gradient]") {
    const std::string src = "4.2/Effects/2000FX/Border/Wipe Border Diamonds";
    std::string why;
    auto e = load(src, &why);
    INFO(why);
    REQUIRE(e);
    REQUIRE(e->frameCount() == 1);
    REQUIRE(e->usesDvMemory() == false);
    const vt::Frame a = pattern(5), b = pattern(6);
    REQUIRE(renderAt(*e, 0, a, b).px == a.px);
    REQUIRE(renderAt(*e, 511, a, b).px == b.px);

    // timing is the fixed (frame-count) timing of the crouton, whatever its TYPE says
    auto c = vt::loadCrouton(golden::sourcePath(src));
    REQUIRE(c);
    const vt::Timing want = vt::fixedTiming(*c);
    REQUIRE(e->timing().durationFrames == want.durationFrames);
    REQUIRE(e->timing().defaultSpeed == want.defaultSpeed);
    REQUIRE(e->timing().fieldsPerFrame == want.fieldsPerFrame);
    REQUIRE(e->name() == "Wipe Border Diamonds");

    const auto g = openGradient(src);
    requireGradientEnds(*e, g);
    for (int t : {1, 60, 200, 255, 256, 400, 510}) {
        INFO("tbar " << t);
        requireGradient(*e, g, t);
    }
}

TEST_CASE("gradient wipe reveals bright areas first and only ever adds B", "[effect][gradient]") {
    auto e = load("4.2/Effects/2000FX/Border/Wipe Border Diamonds");
    REQUIRE(e);
    const auto g = openGradient("4.2/Effects/2000FX/Border/Wipe Border Diamonds");
    const vt::Frame a = solid(kA), b = solid(kB);
    std::vector<uint8_t> shownB(size_t(vt::kRasterW) * vt::kRasterH, 0);
    size_t prevCount = 0;
    for (int t = 0; t < 512; t += 16) {
        const vt::Frame o = renderAt(*e, t, a, b);
        size_t count = 0;
        for (size_t i = 0; i < o.px.size(); ++i) {
            REQUIRE((same(o.px[i], kA) || same(o.px[i], kB)));  // hard edge: no invented colours
            const bool isB = same(o.px[i], kB);
            if (shownB[i]) REQUIRE(isB);  // once revealed, stays revealed
            shownB[i] = isB;
            count += isB;
        }
        REQUIRE(count >= prevCount);
        prevCount = count;
    }
    // the brightest gray level is revealed before the darkest
    const int brightest = int(std::max_element(g.gray.begin(), g.gray.end()) - g.gray.begin());
    const int darkest = int(std::min_element(g.gray.begin(), g.gray.end()) - g.gray.begin());
    size_t pb = 0, pd = 0;
    for (size_t i = 0; i < g.img.px.size(); ++i) {
        if (g.img.px[i] == brightest) pb = i;
        if (g.img.px[i] == darkest) pd = i;
    }
    const int tb = [&] {
        for (int t = 0; t < 512; t += 8)
            if (same(renderAt(*e, t, a, b).at(int(pb % 768), int(pb / 768)), kB)) return t;
        return 512;
    }();
    const int td = [&] {
        for (int t = 0; t < 512; t += 8)
            if (same(renderAt(*e, t, a, b).at(int(pd % 768), int(pd / 768)), kB)) return t;
        return 512;
    }();
    REQUIRE(tb < td);
}

TEST_CASE("gradient softness ramps the edge over that many gray levels", "[effect][gradient]") {
    const std::string src = "4.2/Effects/2000FX/Border/Wipe Border Diamonds";
    auto e = load(src);
    REQUIRE(e);
    const auto g = openGradient(src);
    const vt::Frame a = solid(kA), b = solid(kB);
    vt::RenderCtx c = ctxWith();
    c.guesses.gradientSoftness = 24;
    requireGradientEnds(*e, g, 24);
    bool partial = false;
    for (const auto& p : renderAt(*e, 250, a, b, c).px)
        if (!same(p, kA) && !same(p, kB)) partial = true;
    REQUIRE(partial);
    for (int t : {100, 250, 400}) requireGradient(*e, g, t, 24);
    for (int t : {100, 250, 400}) requireGradient(*e, g, t, 0);
}

namespace {

struct Mix {
    std::map<std::string, golden::Record> byName;
    std::vector<std::string> order;
};
const Mix& mixRecords() {
    static const Mix m = [] {
        Mix r;
        for (auto& rec : golden::readLines("mixtables.jsonl")) {
            r.order.push_back(str(rec, "source"));
            r.byName.emplace(str(rec, "source"), rec);
        }
        return r;
    }();
    return m;
}

// Expected pixel for a golden mixtables record (first palette only: use it for effects whose
// palette does not change). An Overlays-drawer effect of the matte-less kind shows A under its
// art (Ruling 32; the converter knows nothing of drawers). Indices with a tag 0x1B route show
// their source instead (the converter knows nothing of the tag): A, B, or the generator, here
// the default render context's white.
vt::Rgba8 goldenPixel(const golden::Record& r, int idx, int frame, int tbar, vt::Rgba8 pa = kA,
                      vt::Rgba8 pb = kB, bool drawerOverlay = false,
                      const std::vector<vt::PaletteRoute>& routes = {}) {
    if (size_t(idx) < routes.size()) {
        switch (routes[size_t(idx)]) {
            case vt::PaletteRoute::A: return pa;
            case vt::PaletteRoute::B: return pb;
            case vt::PaletteRoute::Generator: return {kWhite.r, kWhite.g, kWhite.b, pa.a};
            case vt::PaletteRoute::Palette: break;
        }
    }
    const std::string& kind = str(r, "kind");
    const auto& over_rgba = arr(r, "over_rgba");
    int level;
    if (kind == "overlay") {
        const int peak = int(num(r, "flash_peak"));
        level = drawerOverlay ? 0 : peak >= 0 ? (frame >= peak ? 255 : 0) : (tbar >= 256 ? 255 : 0);
    } else {
        level = int(arr(r, "lut").at(size_t(idx)));
    }
    vt::Rgba8 m = lerpPx(pa, pb, level);
    if (size_t(idx) * 4 + 3 < over_rgba.size() && over_rgba[size_t(idx) * 4 + 3] != 0) {
        vt::Rgba8 col{uint8_t(over_rgba[size_t(idx) * 4]), uint8_t(over_rgba[size_t(idx) * 4 + 1]),
                      uint8_t(over_rgba[size_t(idx) * 4 + 2]), uint8_t(over_rgba[size_t(idx) * 4 + 3])};
        m = over(m, col);
    }
    return m;
}

void requireAnimGolden(const std::string& source, int tbar) {
    INFO(source << " tbar " << tbar);
    const auto& rec = mixRecords().byName.at(source);
    std::string why;
    auto e = load(source, &why);
    INFO(why);
    REQUIRE(e);
    auto af = openAnim(source);
    const int n = int(af.anim.frames.size());
    REQUIRE(e->frameCount() == n);
    const int frame = vt::animFrameForTBar(tbar, n);
    const auto& f = af.anim.frames[size_t(frame)];
    const Ctl ctl{f.w, f.h};
    const vt::Frame out = renderAt(*e, tbar, solid(kA), solid(kB));
    requireFrame(out, [&](int x, int y) {
        const int line = ctl.line(y);
        if (line < 0) return kBlack;
        const int idx = f.px[size_t(line) * size_t(f.w) + size_t(ctl.x(x))];
        return goldenPixel(rec, idx, frame, tbar, kA, kB, e->isOverlay(), af.plan.routes);
    });
}

}  // namespace

TEST_CASE("Bear-L equals A at its first frame and B at its last, except overlay colours", "[effect][anim]") {
    const std::string src = "4.2/Effects/2000FX/Animals/Bear-L";
    const auto& rec = mixRecords().byName.at(src);
    REQUIRE(str(rec, "kind") == "roles");
    auto e = load(src);
    REQUIRE(e);
    auto af = openAnim(src);
    const int n = int(af.anim.frames.size());
    REQUIRE(e->frameCount() == n);
    REQUIRE(e->timing().durationFrames == vt::animTiming(af.crouton, n).durationFrames);
    REQUIRE(e->timing().fieldsPerFrame == vt::animTiming(af.crouton, n).fieldsPerFrame);
    const int aIdx = int(arr(rec, "detail")[0]), bIdx = int(arr(rec, "detail")[1]);
    const auto& over_rgba = arr(rec, "over_rgba");

    for (int pass = 0; pass < 2; ++pass) {
        const int tbar = pass == 0 ? 0 : 511;
        const int frame = pass == 0 ? 0 : n - 1;
        REQUIRE(vt::animFrameForTBar(tbar, n) == frame);
        const auto& f = af.anim.frames[size_t(frame)];
        // textured sources: every pixel differs, so reading the wrong pixel (or clip) shows up
        const vt::Frame a = pattern(1), b = pattern(2);
        const vt::Frame& video = pass == 0 ? a : b;
        const vt::Frame out = renderAt(*e, tbar, a, b);
        requireFrame(out, [&](int x, int y) {
            const int idx = f.px[size_t(y / 2) * size_t(f.w) + size_t(x)];
            if (over_rgba[size_t(idx) * 4 + 3] != 0) {
                // opaque overlay colour; the alpha under it carries no meaning, so it is not checked
                return vt::Rgba8{uint8_t(over_rgba[size_t(idx) * 4]), uint8_t(over_rgba[size_t(idx) * 4 + 1]),
                                 uint8_t(over_rgba[size_t(idx) * 4 + 2]), out.at(x, y).a};
            }
            // the frame holds only the video index of its own end (and overlay)
            REQUIRE(idx == (pass == 0 ? aIdx : bIdx));
            return video.at(x, y);
        });
    }
    // somewhere in the middle the overlay is on screen and both clips show
    bool sawOverlay = false;
    for (int t = 0; t < 512 && !sawOverlay; t += 32) {
        const auto o = renderAt(*e, t, solid(kA), solid(kB));
        for (const auto& p : o.px)
            if (!same(p, kA) && !same(p, kB)) sawOverlay = true;
    }
    REQUIRE(sawOverlay);
}

TEST_CASE("RenderCtx::animFrame picks the frame instead of the T-bar", "[effect][anim]") {
    // an animated wipe and a DVE move; neither depends on the T-bar except through the frame
    for (const char* src : {"4.2/Effects/2000FX/Animals/Bear-L", "4.2/Effects/2000FX/Blinds/Split Corners In"}) {
        INFO(src);
        auto e = load(src);
        REQUIRE(e);
        const int n = e->frameCount();
        REQUIRE(n > 2);
        const vt::Frame a = pattern(1), b = pattern(2);
        for (int k : {0, n / 3, n - 1}) {
            vt::RenderCtx c = ctxWith();
            c.animFrame = k;
            REQUIRE(renderAt(*e, 0, a, b, c).px == renderAt(*e, tbarForFrame(k, n), a, b).px);
        }
        vt::RenderCtx c = ctxWith();
        c.animFrame = n + 5;  // past the end: the last frame
        REQUIRE(renderAt(*e, 0, a, b, c).px == renderAt(*e, 511, a, b).px);
    }
}

TEST_CASE("a 384-wide effect maps control pixel (10, 5) to raster (20..21, 10..11)", "[effect][anim]") {
    const std::string src = "4.2/Effects/2000FX/Special/Clapboard Matte";
    const auto& rec = mixRecords().byName.at(src);
    auto e = load(src);
    REQUIRE(e);
    auto af = openAnim(src);
    const int n = int(af.anim.frames.size());
    REQUIRE(af.anim.bm.w == 384);
    REQUIRE(af.anim.bm.h == 241);

    // A black, B white: a clip pixel's level comes out as its red channel
    const vt::Frame black = solid(kBlack), white = solid(kWhite);
    for (int frame : {n / 4, n / 2, n - 1}) {
        const auto& f = af.anim.frames[size_t(frame)];
        const int tbar = tbarForFrame(frame, n);
        const vt::Frame out = renderAt(*e, tbar, black, white);
        auto ctrl = [&](int cx, int cy) {
            return goldenPixel(rec, f.px[size_t(cy) * 384 + size_t(cx)], frame, tbar, kBlack, kWhite, false, af.plan.routes);
        };
        // control pixel (10, 5) covers exactly raster (20..21, 10..11)
        for (int y = 10; y <= 11; ++y)
            for (int x = 20; x <= 21; ++x) REQUIRE(same(out.at(x, y), ctrl(10, 5)));
        // and the whole picture follows the same rule, control line 240 on lines 480-481
        for (int y = 0; y < vt::kRasterH; ++y)
            for (int x = 0; x < vt::kRasterW; ++x)
                if (!same(out.at(x, y), ctrl(x / 2, y / 2))) FAIL("mismatch at " << x << "," << y);
    }
}

TEST_CASE("raster lines with no control line render black, never a copy", "[effect][anim]") {
    // 480- and 240-line effects drive 480 raster lines; lines 480-481 have no control line
    const auto recs = golden::readLines("anims.jsonl");
    int checked = 0;
    for (const auto& r : recs) {
        const int64_t h = num(r, "h");
        if (h != 480 && h != 240) continue;
        const std::string& src = str(r, "source");
        INFO(src);
        std::string why;
        auto e = load(src, &why);
        if (!e) continue;  // DVE etc.
        ++checked;
        for (int t : {0, 300, 511}) {
            const vt::Frame out = renderAt(*e, t, solid(kA), solid(kB));
            for (int y = 480; y < 482; ++y)
                for (int x = 0; x < vt::kRasterW; ++x) REQUIRE(same(out.at(x, y), kBlack));
        }
    }
    REQUIRE(checked >= 8);
    // 241-line effects fill every raster line
    auto e = load("4.2/Effects/2000FX/Animals/Bear-L");
    REQUIRE(e);
    const vt::Frame out = renderAt(*e, 0, solid(kA), solid(kB));
    for (int x = 0; x < vt::kRasterW; ++x) REQUIRE(same(out.at(x, 481), out.at(x, 480)));
}

TEST_CASE("animated effects of every control image shape match the mix model", "[effect][anim][golden]") {
    // 768x241 / 384x241 / 768x482 / 768x480 / 384x240 / 768x240
    const char* names[] = {
        "4.2/Effects/2000FX/Animals/Bear-L",          "4.2/Effects/2000FX/Special/Clapboard Matte",
        "4.2/Effects/2000FX/Border/Border Flip In",   "4.2/Effects/2000FX/Organic/Flare Center",
        "4.2/Effects/2000FX/Organic/Organic Burble",  "4.2/Effects/4000FX/Overlays/Funky !",
        "4.2/Effects/4000FX/Organic/Flare TR-BL !",
    };
    for (const char* s : names) {
        REQUIRE(mixRecords().byName.count(s) == 1);
        for (int t : {0, 130, 256, 380, 511}) requireAnimGolden(s, t);
    }
}

TEST_CASE("flash effects show A before the peak and B from it, overlay on top", "[effect][anim][flash]") {
    int tested = 0;
    for (const auto& src : mixRecords().order) {
        const auto& r = mixRecords().byName.at(src);
        if (str(r, "kind") != "overlay" || num(r, "flash_peak") < 0) continue;
        INFO(src);
        auto e = load(src);
        REQUIRE(e);
        auto af = openAnim(src);
        const int n = int(af.anim.frames.size());
        const int peak = vt::flashPeak(af.anim, af.plan);
        REQUIRE(peak == int(num(r, "flash_peak")));
        REQUIRE(peak >= 0);
        for (int frame : {std::max(0, peak - 1), peak}) {
            const int tbar = tbarForFrame(frame, n);
            const vt::Frame out = renderAt(*e, tbar, solid(kA), solid(kB));
            const vt::Rgba8 want = frame >= peak ? kB : kA;
            const auto& f = af.anim.frames[size_t(frame)];
            const Ctl ctl{f.w, f.h};
            const auto& over_rgba = arr(r, "over_rgba");
            for (int y = 0; y < vt::kRasterH; y += 3)
                for (int x = 0; x < vt::kRasterW; x += 3) {
                    const int line = ctl.line(y);
                    if (line < 0) continue;
                    const int idx = f.px[size_t(line) * size_t(f.w) + size_t(ctl.x(x))];
                    if (size_t(idx) * 4 + 3 < over_rgba.size() && over_rgba[size_t(idx) * 4 + 3] != 0) continue;
                    if (!same(out.at(x, y), want)) FAIL("frame " << frame << " at " << x << "," << y);
                }
        }
        ++tested;
    }
    REQUIRE(tested >= 1);
}

namespace {

// The mix model from mixTable, with the palette rule (most recent non-empty cmaps[j], 1 <= j <= frame).
void requireAnimModel(const AnimFile& af, const vt::Effect& e, int tbar) {
    const int n = int(af.anim.frames.size());
    const int frame = vt::animFrameForTBar(tbar, n);
    const std::vector<vt::Rgb>* cm = &af.anim.bm.cmap;
    for (int j = 1; j <= frame; ++j)
        if (j < int(af.anim.cmaps.size()) && af.anim.cmaps[size_t(j)] && !af.anim.cmaps[size_t(j)]->empty())
            cm = &*af.anim.cmaps[size_t(j)];
    const auto table = vt::mixTable(af.anim, af.plan, *cm, kWhite);
    const auto& f = af.anim.frames[size_t(frame)];
    const Ctl ctl{f.w, f.h};
    const int peak = vt::flashPeak(af.anim, af.plan);
    const vt::Frame out = renderAt(e, tbar, solid(kA), solid(kB));
    requireFrame(out, [&](int x, int y) {
        const int line = ctl.line(y);
        if (line < 0) return kBlack;
        const auto& en = table[f.px[size_t(line) * size_t(f.w) + size_t(ctl.x(x))]];
        int level = en.bLevel;
        if (af.plan.kind == vt::AnimKind::Overlay)  // a drawer overlay shows A under its art (Ruling 32)
            level = e.isOverlay() ? 0 : peak >= 0 ? (frame >= peak ? 255 : 0) : (tbar >= 256 ? 255 : 0);
        vt::Rgba8 m = lerpPx(kA, kB, level);
        if (en.src == vt::MixSrc::Colour) m = over(m, en.colour);
        return m;
    });
}

}  // namespace

TEST_CASE("palette changes mid-effect use the most recent CMAP", "[effect][anim]") {
    int withChange = 0;
    for (const char* src : {"4.2/Effects/4000FX/Overlays/Atom !", "4.2/Effects/4000FX/Overlays/20 OFF !"}) {
        INFO(src);
        auto af = openAnim(src);
        auto e = load(src);
        REQUIRE(e);
        const int n = int(af.anim.frames.size());
        int change = -1;  // a frame that changes the palette
        for (size_t k = 1; k < af.anim.cmaps.size(); ++k)
            if (af.anim.cmaps[k] && !af.anim.cmaps[k]->empty()) {
                change = int(k);
                break;
            }
        REQUIRE(change >= 1);
        ++withChange;
        for (int frame : {change - 1, change, std::min(n - 1, change + 1), n - 1}) requireAnimModel(af, *e, tbarForFrame(frame, n));
    }
    REQUIRE(withChange == 2);
}

TEST_CASE("black palettes use the background colour from the render context", "[effect][anim]") {
    // Bear-L's palette has colours; find an effect whose overlay palette is all black instead
    bool found = false;
    for (const auto& src : mixRecords().order) {
        auto af = openAnim(src);
        if (af.plan.kind == vt::AnimKind::Soft) continue;
        const auto w = vt::mixTable(af.anim, af.plan, af.anim.bm.cmap, kWhite);
        const auto r = vt::mixTable(af.anim, af.plan, af.anim.bm.cmap, vt::bkgColour(vt::BkgColour::Red));
        bool differs = false;
        for (size_t i = 0; i < w.size(); ++i)
            if (w[i].src == vt::MixSrc::Colour && !same(w[i].colour, r[i].colour)) differs = true;
        if (!differs) continue;
        auto e = load(src);
        if (!e) continue;
        found = true;
        const vt::Rgba8 red = vt::bkgColour(vt::BkgColour::Red);
        const vt::Frame o1 = renderAt(*e, 100, solid(kA), solid(kB), ctxWith(kWhite));
        const vt::Frame o2 = renderAt(*e, 100, solid(kA), solid(kB), ctxWith(red));
        const vt::Frame o3 = renderAt(*e, 100, solid(kA), solid(kB), ctxWith(kWhite));
        REQUIRE(o1.px != o2.px);
        REQUIRE(o1.px == o3.px);
        break;
    }
    REQUIRE(found);
}

TEST_CASE("tag 0x1B routes the palette indices of 2-plane wipes to A, B or the colour generator", "[effect][anim][colour]") {
    // One named effect per cause (tools/re/FINDINGS.md "Which colour an effect's matte shows").
    struct Case {
        const char* src;
        int idx;
        vt::PaletteRoute route;
    };
    const Case named[] = {
        // a black palette index the converter's rules make matte colour is B on the hardware
        {"4.2/Effects/2000FX/Wipes/Square B-T", 3, vt::PaletteRoute::B},
        // a border in a preview palette colour (blue) is the generator (tag 0x23: White)
        {"4.2/Effects/2000FX/Border/Border BR-TL", 3, vt::PaletteRoute::Generator},
        // black notes in a palette that has colours are the generator too (White)
        {"4.2/Effects/2000FX/Special/Flying Notes", 3, vt::PaletteRoute::Generator},
    };
    const vt::Rgba8 red = vt::bkgColour(vt::BkgColour::Red);
    for (const auto& c : named) {
        INFO(c.src);
        auto af = openAnim(c.src);
        REQUIRE(af.plan.routes.size() == 4);
        REQUIRE(af.plan.routes[size_t(c.idx)] == c.route);
        // the converter's palette rules alone draw a colour there: the matte colour stand-in
        // (Square B-T, whose missing tag 0x23 made it black) or a palette colour
        vt::AnimPlan plain = af.plan;
        plain.routes.clear();
        const auto t = vt::mixTable(af.anim, plain, af.anim.bm.cmap, red);
        REQUIRE(t[size_t(c.idx)].src == vt::MixSrc::Colour);
        if (c.route == vt::PaletteRoute::Generator) REQUIRE_FALSE(same(t[size_t(c.idx)].colour, {red.r, red.g, red.b, 255}));
    }
    REQUIRE(load(named[1].src)->effectColour() == vt::EffectColour::of(vt::BkgColour::White));
    REQUIRE(load(named[2].src)->effectColour() == vt::EffectColour::of(vt::BkgColour::White));

    // Every routed index of every routed effect renders its source alone.
    int routed = 0;
    const vt::Frame a = pattern(1), b = pattern(2);
    for (const auto& src : mixRecords().order) {
        auto af = openAnim(src);
        if (af.plan.routes.empty()) continue;
        INFO(src);
        auto e = load(src);
        REQUIRE(e);
        ++routed;
        const int n = int(af.anim.frames.size());
        for (int frame : {0, n / 2, n - 1}) {
            const int tbar = tbarForFrame(frame, n);
            const vt::Frame out = renderAt(*e, tbar, a, b, ctxWith(red));
            const auto& f = af.anim.frames[size_t(frame)];
            const Ctl ctl{f.w, f.h};
            for (int y = 0; y < vt::kRasterH; y += 3)
                for (int x = 0; x < vt::kRasterW; x += 3) {
                    const int line = ctl.line(y);
                    if (line < 0) continue;
                    const int idx = f.px[size_t(line) * size_t(f.w) + size_t(ctl.x(x))];
                    const vt::Rgba8 got = out.at(x, y);
                    switch (af.plan.routes[size_t(idx)]) {
                        case vt::PaletteRoute::Palette: break;
                        case vt::PaletteRoute::A:
                            if (!same(got, a.at(x, y))) FAIL("frame " << frame << " at " << x << "," << y << ": not A");
                            break;
                        case vt::PaletteRoute::B:
                            if (!same(got, b.at(x, y))) FAIL("frame " << frame << " at " << x << "," << y << ": not B");
                            break;
                        case vt::PaletteRoute::Generator:
                            if (!same(got, {red.r, red.g, red.b, a.at(x, y).a}))
                                FAIL("frame " << frame << " at " << x << "," << y << ": not the generator");
                            break;
                    }
                }
        }
    }
    REQUIRE(routed == 125);  // the distinct 2-plane wipes with a tag 0x1B (the golden list skips duplicates)
}

TEST_CASE("loadEffect refuses what it cannot play, with a reason", "[effect][factory]") {
    std::string why;
    // Bar pictures taller than the raster (Ruling 11)
    int bars = 0;
    for (const auto& r : golden::readLines("ilbm.jsonl")) {
        if (num(r, "h") <= 482) continue;
        why.clear();
        INFO(str(r, "source"));
        REQUIRE(load(str(r, "source"), &why) == nullptr);
        REQUIRE(why == "bar picture moved by Switcher code (not reverse-engineered yet)");
        ++bars;
    }
    REQUIRE(bars == 4);
    // a missing file and a non-crouton
    why.clear();
    REQUIRE(vt::loadEffect(dataRoot() / "no such effect", &why) == nullptr);
    REQUIRE(!why.empty());
    REQUIRE(vt::loadEffect(dataRoot() / "no such effect") == nullptr);  // whyNot is optional
}

namespace {

// Every crouton of one TYPE under 4.2/Effects, or another folder of the data tree (icons skipped).
std::vector<std::filesystem::path> croutonsOfType(vt::CroutonType type, const char* folder = "4.2/Effects") {
    std::vector<std::filesystem::path> out;
    for (auto& ent : std::filesystem::recursive_directory_iterator(dataRoot() / folder)) {
        if (!ent.is_regular_file()) continue;
        auto ext = ent.path().extension().string();
        if (ext == ".i" || ext == ".I") continue;
        auto c = vt::loadCrouton(ent.path());
        if (c && c->type == type) out.push_back(ent.path());
    }
    std::sort(out.begin(), out.end());
    return out;
}

}  // namespace

TEST_CASE("MAIN and CTRL croutons are Switcher moves", "[effect][factory]") {
    int seen = 0;
    for (auto [type, name] : {std::pair{vt::CroutonType::Main, "MAIN"}, std::pair{vt::CroutonType::Ctrl, "CTRL"}})
        for (const auto& p : croutonsOfType(type)) {
            ++seen;
            std::string why;
            INFO(p.string());
            REQUIRE(vt::loadEffect(p, &why) == nullptr);
            REQUIRE(why == std::string("Switcher move (") + name + ") not reverse-engineered yet");
        }
    REQUIRE(seen > 0);
}

// ALGO.md 3 counts 462 ALGO files in 4.2: 230 croutons and their 232 icons (the .i files carry
// the tags too). Of the croutons, 2 are Art Card Key (4 with the icons) and 228 play.
TEST_CASE("every ALGO crouton but Art Card Key loads", "[effect][factory][algo]") {
    int loaded = 0, artCardKey = 0;
    for (const auto& p : croutonsOfType(vt::CroutonType::Algo, "4.2")) {
        std::string why;
        INFO(p.string());
        const auto c = vt::loadCrouton(p);
        if (c->tag(0x80000063).value_or(0) == 5) {
            REQUIRE(vt::loadEffect(p, &why) == nullptr);
            REQUIRE(why == "Art Card Key needs keys (not built yet)");
            ++artCardKey;
            continue;
        }
        INFO(why);
        REQUIRE(vt::loadEffect(p, &why) != nullptr);
        ++loaded;
    }
    REQUIRE(artCardKey == 2);
    REQUIRE(loaded == 228);
}

namespace {

// Coordinate gradients: every pixel differs from the other picture's and from its neighbours,
// and the alpha says which picture a pixel came from (renderDve keeps the source alpha).
vt::Frame algoPicture(bool b) {
    vt::Frame f;
    for (int y = 0; y < vt::kRasterH; ++y)
        for (int x = 0; x < vt::kRasterW; ++x) {
            const int r = x & 255, g = y & 255, hi = (x >> 8) * 4 + (y >> 8);
            f.at(x, y) = b ? vt::Rgba8{uint8_t(255 - r), uint8_t(255 - g), uint8_t(200 + hi), 251}
                           : vt::Rgba8{uint8_t(r), uint8_t(g), uint8_t(hi), 250};
        }
    return f;
}

// "" if the frames agree in kVisibleArea, else the first difference.
std::string visibleDiff(const vt::Frame& got, const vt::Frame& want) {
    const vt::Rect r = vt::kVisibleArea;
    for (int y = r.y; y < r.y + r.h; ++y)
        for (int x = r.x; x < r.x + r.w; ++x)
            if (!same(got.at(x, y), want.at(x, y))) return "first difference at (" + std::to_string(x) + "," + std::to_string(y) + ")";
    return "";
}

// The first ALGO crouton under 4.2/Effects with this file name.
std::filesystem::path algoNamed(const std::string& name) {
    static const auto all = croutonsOfType(vt::CroutonType::Algo);
    for (const auto& p : all)
        if (p.filename().string() == name) return p;
    return {};
}

std::unique_ptr<vt::Effect> loadAlgo(const std::string& name) {
    const auto p = algoNamed(name);
    INFO(name);
    REQUIRE(!p.empty());
    std::string why;
    auto e = vt::loadEffect(p, &why);
    INFO(why);
    REQUIRE(e);
    return e;
}

vt::Frame algoRender(const vt::Effect& e, int tbar, uint64_t take, int autoStep = -1) {
    vt::RenderCtx c;
    c.take = take;
    c.autoStep = autoStep;
    static const vt::Frame a = algoPicture(false), b = algoPicture(true);
    vt::Frame out;
    e.render(tbar, a, b, out, c);
    return out;
}

}  // namespace

TEST_CASE("algo moves start and end on the right picture", "[algo]") {
    const vt::Frame a = algoPicture(false), b = algoPicture(true);
    for (const char* name : {"Crop To Center", "Squeeze T-B"}) {  // descriptor 3: A moves over B
        INFO(name);
        const auto e = loadAlgo(name);
        REQUIRE(visibleDiff(algoRender(*e, 0, 1), a) == "");
    }
    // Descriptor 2: B moves over A, and fills the screen at the end.
    std::string push = "Push On L-R";
    if (algoNamed(push).empty())
        for (const auto& p : croutonsOfType(vt::CroutonType::Algo)) {
            const auto c = vt::loadCrouton(p);
            const auto dir = p.parent_path().filename().string();
            if ((dir == "Wipes" || dir == "Digital") && c->tag(0x80000007).value_or(0) == 2) {
                push = p.filename().string();
                break;
            }
        }
    {
        INFO(push);
        const auto e = loadAlgo(push);
        REQUIRE(visibleDiff(algoRender(*e, 511, 1), b) == "");
        REQUIRE(visibleDiff(algoRender(*e, 511, 2), b) == "");
    }
    // Every wipe starts on A and ends on B.
    int wipes = 0;
    std::vector<std::string> failures;
    for (const auto& p : croutonsOfType(vt::CroutonType::Algo)) {
        if (vt::loadCrouton(p)->tag(0x80000063).value_or(0) != 1) continue;
        ++wipes;
        const auto e = vt::loadEffect(p);
        REQUIRE(e);
        for (auto [tbar, want] : {std::pair{0, &a}, std::pair{511, &b}})
            if (auto d = visibleDiff(algoRender(*e, tbar, 1), *want); !d.empty())
                failures.push_back(p.filename().string() + " at T-bar " + std::to_string(tbar) + ": " + d);
    }
    for (const auto& f : failures) WARN(f);
    REQUIRE(wipes > 0);
    CHECK(failures.empty());
}

// Ruling 9 is for a file that exists: a missing path is refused even with an ALGO icon beside it,
// and a readable file with no CrUD plays its icon's program.
TEST_CASE("icon-file fallback needs the effect file to exist", "[algo]") {
    namespace fs = std::filesystem;
    REQUIRE(vt::loadEffect(dataRoot() / "4.2/Effects/4000FX/no such effect") == nullptr);
    const fs::path tmp = fs::temp_directory_path() / "vt_icon_fallback";
    fs::create_directories(tmp);
    fs::copy_file(dataRoot() / "4.2/Effects/4000FX/Smooth Fade.I", tmp / "Fade.I", fs::copy_options::overwrite_existing);
    std::string why;
    REQUIRE(vt::loadEffect(tmp / "Fade", &why) == nullptr);  // no Fade file
    std::ofstream(tmp / "Fade", std::ios::binary) << "not a crouton";
    REQUIRE(vt::loadEffect(tmp / "Fade", &why) != nullptr);  // exists, no CrUD
    fs::remove_all(tmp);
}

// Ruling 9: the plain Smooth Fade files hold no CrUD; the program is in the sibling .I file.
TEST_CASE("algo program read from the icon file (Smooth Fade)", "[algo]") {
    const vt::Frame b = algoPicture(true);
    for (const char* dir : {"4.2/Effects/2000FX", "4.2/Effects/4000FX"}) {
        const auto p = dataRoot() / dir / "Smooth Fade";
        INFO(p.string());
        std::string why;
        const auto e = vt::loadEffect(p, &why);
        INFO(why);
        REQUIRE(e);
        REQUIRE(e->name() == "Smooth Fade");
        // A fade writes opaque pixels (mixLevel), so compare against opaque pictures.
        vt::Frame a = algoPicture(false), bo = b, out;
        for (auto& px : a.px) px.a = 255;
        for (auto& px : bo.px) px.a = 255;
        vt::RenderCtx c;
        c.take = 1;
        e->render(511, a, bo, out, c);
        REQUIRE(visibleDiff(out, bo) == "");
    }
}

TEST_CASE("algo take resets", "[algo]") {
    // The history 100 -> 300 reaches V[239] (ALGO.md 15.1, raster lines 478..479, outside the
    // visible area), so its frame at 300 differs from a fresh take's. Without a reset, take 2 would
    // reuse F1's step (same key) and F2 would be F1.
    const vt::Frame fresh = algoRender(*loadAlgo("Squeeze T-B"), 300, 1);
    const auto e = loadAlgo("Squeeze T-B");
    algoRender(*e, 100, 1);
    const vt::Frame f1 = algoRender(*e, 300, 1);
    const vt::Frame f2 = algoRender(*e, 300, 2);
    algoRender(*e, 100, 3);
    const vt::Frame f3 = algoRender(*e, 300, 3);
    REQUIRE(f1.px != fresh.px);
    REQUIRE(visibleDiff(f1, fresh) == "");
    REQUIRE(f2.px == fresh.px);
    REQUIRE(f3.px == f1.px);
}

TEST_CASE("algo repeated render does not step", "[algo]") {
    const auto e = loadAlgo("Squeeze T-B");
    const vt::Frame f = algoRender(*e, 300, 1);
    REQUIRE(algoRender(*e, 300, 1).px == f.px);
    REQUIRE(algoRender(*e, 300, 1).px == f.px);
    // Descriptor 5: A over A below T-bar 256, B over B from 256 (ALGO.md 15.2).
    std::string name = algoNamed("Stretch").empty() ? "Infinity Slide" : "Stretch";
    const auto d5 = loadAlgo(name);
    INFO(name);
    auto count = [](const vt::Frame& f, uint8_t alpha) {
        return std::count_if(f.px.begin(), f.px.end(), [&](const vt::Rgba8& p) { return p.a == alpha; });
    };
    const vt::Frame before = algoRender(*d5, 255, 1), after = algoRender(*d5, 256, 1);
    REQUIRE(count(before, 250) > 0);
    REQUIRE(count(before, 251) == 0);
    REQUIRE(count(after, 251) > 0);
    REQUIRE(count(after, 250) == 0);
}

TEST_CASE("every corpus animated and gradient wipe loads and renders", "[effect][corpus]") {
    int loaded = 0, sounds = 0;
    std::set<std::string> reasons;
    // 3.1 and 4.2 animated wipes (the mixtables list is every non-DVE ANIM file)
    for (const auto& src : mixRecords().order) {
        INFO(src);
        std::string why;
        auto e = load(src, &why);
        if (!e) {
            reasons.insert(why);
            continue;
        }
        ++loaded;
        const auto af = openAnim(src);
        REQUIRE(e->frameCount() == int(af.anim.frames.size()));
        REQUIRE(e->timing().durationFrames == vt::animTiming(af.crouton, e->frameCount()).durationFrames);
        // sound: present exactly when the file has a decodable 8SVX
        std::optional<vt::Sound> s;
        try {
            s = vt::readSound(af.crouton.bytes);
        } catch (const std::exception&) {
        }
        REQUIRE((e->sound() != nullptr) == s.has_value());
        if (e->sound()) {
            ++sounds;
            REQUIRE(e->sound()->pcm == s->pcm);
        }
        for (int t : {0, 256, 511}) requireAnimModel(af, *e, t);
    }
    for (const auto& r : golden::readLines("ilbm.jsonl")) {
        const std::string& src = str(r, "source");
        INFO(src);
        std::string why;
        auto e = load(src, &why);
        if (!e) {
            REQUIRE(num(r, "h") > 482);
            continue;
        }
        ++loaded;
        auto c = vt::loadCrouton(golden::sourcePath(src));
        REQUIRE(c);
        const auto g = openGradient(src);
        const bool hasAnimForm = firstForm(*c, kAnim).has_value();
        if (!hasAnimForm) REQUIRE(e->timing().durationFrames == vt::fixedTiming(*c).durationFrames);
        requireGradientEnds(*e, g);
        requireGradient(*e, g, 300);
    }
    INFO("refused reasons: " << (reasons.empty() ? "none" : *reasons.begin()));
    REQUIRE(loaded > 350);
    REQUIRE(sounds >= 1);
    // everything refused among the animated wipes has a known reason
    for (const auto& why : reasons) REQUIRE(why == "overlay effect with nothing but video in it");
}

TEST_CASE("engine warnings go to the log sink (setLogSink)", "[effect][log]") {
    // Falling Sheep with its 8SVX marked compressed: the effect loads without sound and warns.
    std::vector<uint8_t> bytes;
    {
        std::ifstream in(golden::sourcePath("4.2/Effects/2000FX/Animals/Falling Sheep"), std::ios::binary);
        bytes.assign(std::istreambuf_iterator<char>(in), {});
    }
    const std::string svx = "8SVX", vhdr = "VHDR";
    const auto s = std::search(bytes.begin(), bytes.end(), svx.begin(), svx.end());
    REQUIRE(s != bytes.end());
    const auto v = std::search(s, bytes.end(), vhdr.begin(), vhdr.end());
    REQUIRE(v != bytes.end());
    v[8 + 15] = 1;  // VHDR sCompression (oneShot, repeat, perCycle: 4 bytes each, rate 2, octaves 1)
    const auto path = std::filesystem::temp_directory_path() / "vtoast_log_sink_test";
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
    }
    std::vector<std::string> got;
    vt::setLogSink([&](std::string_view m) { got.emplace_back(m); });
    auto e = vt::loadEffect(path);
    vt::setLogSink({});  // back to stderr
    std::filesystem::remove(path);
    REQUIRE(e);
    REQUIRE(e->sound() == nullptr);
    REQUIRE(got.size() == 1);
    REQUIRE(got[0].find("vtoast_log_sink_test: sound not loaded") == 0);
    // the untouched file loads with its sound and says nothing
    got.clear();
    vt::setLogSink([&](std::string_view m) { got.emplace_back(m); });
    auto ok = load("4.2/Effects/2000FX/Animals/Falling Sheep");
    vt::setLogSink({});
    REQUIRE(ok);
    REQUIRE(ok->sound() != nullptr);
    REQUIRE(got.empty());
}

TEST_CASE("a matte-less Overlays-drawer effect shows A under its art at every T-bar (Ruling 32)", "[effect][anim]") {
    // 20 OFF ! has no matte (overlay kind); in the drawer its B (the effect-colour fill) never shows
    auto e = load("4.2/Effects/4000FX/Overlays/20 OFF !");
    REQUIRE(e);
    REQUIRE(e->isOverlay());
    auto af = openAnim("4.2/Effects/4000FX/Overlays/20 OFF !");
    REQUIRE(af.plan.kind == vt::AnimKind::Overlay);
    for (int t : {0, 256, 511}) {
        const vt::Frame out = renderAt(*e, t, solid(kA), solid(kB));
        REQUIRE(std::none_of(out.px.begin(), out.px.end(), [](vt::Rgba8 p) { return same(p, kB); }));
    }
}

namespace {

// Distinct, never white: A and B differ everywhere.
vt::Frame ramp(int s) {
    vt::Frame f;
    for (int y = 0; y < vt::kRasterH; ++y)
        for (int x = 0; x < vt::kRasterW; ++x)
            f.at(x, y) = {uint8_t(x * 255 / 767 / s), uint8_t(y * 255 / 481 / s), uint8_t(60 * s), 255};
    return f;
}

const char* const kBangDves[] = {
    "4.2/Effects/4000FX/Digital/Hand Pull !",           "4.2/Effects/4000FX/Digital/Flip Chrome Edge !",
    "4.2/Effects/4000FX/Digital/Warp To Globe !",       "4.2/Effects/4000FX/Digital/Fly Over Crumpled Edge !",
    "4.2/Effects/4000FX/Trajectory/Flip Out Bottom2 !", "4.2/Effects/4000FX/Space/Infinite Video Space !",
    "4.2/Effects/4000FX/Trajectory/Photo Fly Out !",
};

}  // namespace

TEST_CASE("Photo Fly Out ! flashes white, then fades A in", "[dve][effect]") {
    const std::string src = "4.2/Effects/4000FX/Trajectory/Photo Fly Out !";
    auto c = vt::loadCrouton(golden::sourcePath(src));
    REQUIRE(c);
    auto af = firstForm(*c, kAnim);
    REQUIRE(af);
    const vt::Anim an = vt::readAnim(c->bytes, *af);
    REQUIRE(an.planes.size() >= 11);
    auto e = load(src);
    REQUIRE(e);
    const vt::DveInfo info = vt::dveInfo(an);
    REQUIRE((info.role == vt::DveRole::MovingA || info.role == vt::DveRole::MovingB));
    const vt::Frame a = ramp(1), b = ramp(2);
    const vt::Frame* moving = info.role == vt::DveRole::MovingA ? &a : &b;
    const vt::Frame* background = info.role == vt::DveRole::MovingA ? &b : &a;
    vt::RenderCtx ctx = ctxWith(kWhite);
    ctx.generator = nullptr;

    // (384, 240) has plane 4 set in frames 0-6 and L = 0 in frames 0-3, then 1, 2, 3 (Ruling 1 was
    // not needed). Every plane-4 pixel also carries the art layer (art group >= 2, an opaque
    // colour), so d, the picture the mix is taken against, is rendered without the art layer: the
    // mix replaces the art rule at a routed pixel (vt/dve.h), it does not blend with it.
    const int kProbeX = 384, kProbeY = 240;
    const int w[3] = {85, 170, 255};
    for (int k = 0; k <= 10; ++k) {
        INFO("frame " << k);
        ctx.animFrame = k;
        vt::Frame got;
        e->render(0, a, b, got, ctx);
        const vt::DveControl dc = vt::decodeDve(an.planes[size_t(k)]);
        const uint8_t idx = dc.index[size_t(kProbeY / 2) * 768 + size_t(kProbeX)];
        if (k <= 6) REQUIRE((idx & 16) != 0);
        const vt::Rgba8 o = got.at(kProbeX, kProbeY);
        if (k <= 3) {
            REQUIRE(((idx >> 2) & 3) == 0);
            REQUIRE(same(o, kWhite));
        } else if (k <= 6) {
            REQUIRE(((idx >> 2) & 3) == k - 3);
            vt::Frame d;
            vt::DveControl noArt = dc;
            noArt.art.clear();
            vt::renderDve(noArt, *moving, *background, {}, d, ctx.guesses);
            const vt::Rgba8 dp = d.at(kProbeX, kProbeY);
            const int wk = w[k - 4];
            REQUIRE(same(o, {uint8_t((255 * (255 - wk) + dp.r * wk + 127) / 255), uint8_t((255 * (255 - wk) + dp.g * wk + 127) / 255),
                             uint8_t((255 * (255 - wk) + dp.b * wk + 127) / 255), 255}));
        }
    }

    // frame 10: the border ring (plane 4 set, L = 0) is all white
    ctx.animFrame = 10;
    vt::Frame got;
    e->render(0, a, b, got, ctx);
    const vt::DveControl dc = vt::decodeDve(an.planes[10]);
    int ring = 0;
    for (int y = 0; y < vt::kRasterH; ++y)
        for (int x = 0; x < vt::kRasterW; ++x) {
            const uint8_t idx = dc.index[size_t(y / 2) * 768 + size_t(x)];
            if ((idx & 16) && ((idx >> 2) & 3) == 0) {
                ++ring;
                if (!same(got.at(x, y), kWhite)) FAIL("ring pixel (" << x << "," << y << ") is not white");
            }
        }
    REQUIRE(ring >= 1000);

    // Ruling 10: descriptor 3 moves A, so the uncovered area (plane 4 clear, L = 0, routed to
    // S = 1) shows B, the bus that is not moving, from frame 10 to the end
    REQUIRE(c->tag(0x80000007).value_or(0) == 3);
    for (int k : {10, e->frameCount() - 1}) {
        INFO("frame " << k);
        ctx.animFrame = k;
        e->render(0, a, b, got, ctx);
        const vt::DveControl dk = vt::decodeDve(an.planes[size_t(k)]);
        int open = 0;
        for (int y = 0; y < vt::kRasterH; ++y)
            for (int x = 0; x < vt::kRasterW; ++x) {
                const uint8_t idx = dk.index[size_t(y / 2) * 768 + size_t(x)];
                if ((idx & 16) || ((idx >> 2) & 3) != 0) continue;
                ++open;
                if (!same(got.at(x, y), b.at(x, y))) FAIL("open pixel (" << x << "," << y << ") is not B");
            }
        REQUIRE(open >= 1000);
    }
}

TEST_CASE("Flip Out Bottom2 ! opens on A (descriptor 32 moves A, Ruling 10)", "[dve][effect]") {
    const std::string src = "4.2/Effects/4000FX/Trajectory/Flip Out Bottom2 !";
    const auto c = vt::loadCrouton(golden::sourcePath(src));
    REQUIRE(c);
    REQUIRE(c->tag(0x80000007).value_or(0) == 32);
    auto e = load(src);
    REQUIRE(e);
    const vt::Frame a = ramp(1), b = ramp(2);
    vt::RenderCtx ctx = ctxWith(kWhite);
    ctx.animFrame = 0;
    vt::Frame got;
    e->render(0, a, b, got, ctx);
    const int x = vt::kVisibleArea.x + vt::kVisibleArea.w / 2, y = vt::kVisibleArea.y + vt::kVisibleArea.h / 2;
    REQUIRE(same(got.at(x, y), a.at(x, y)));
    REQUIRE(!same(got.at(x, y), b.at(x, y)));
}

// Ruling 10: in the A-moving "!" DVEs a route to S = 3 shows A's DV picture (RenderCtx::digitalA,
// delayed while live) as the move moves it, which is the DVE picture itself, and a route to S = 1
// the analog B. Every L = 0 routed pixel is exactly that.
TEST_CASE("\"!\" DVE routes: S = 3 is the moving bus's moved DV picture, S = 1 the analog other bus", "[dve][effect]") {
    const vt::Frame a = ramp(1), b = ramp(2), d = ramp(3), black = solid(kBlack);
    int s1 = 0, s3 = 0;
    for (const char* src : {"4.2/Effects/4000FX/Trajectory/Photo Fly Out !", "4.2/Effects/4000FX/Digital/Warp To Globe !",
                            "4.2/Effects/4000FX/Trajectory/Flip Out Bottom2 !",
                            "4.2/Effects/4000FX/Digital/Fly Over Crumpled Edge !"}) {
        INFO(src);
        auto c = vt::loadCrouton(golden::sourcePath(src));
        REQUIRE(c);
        const uint32_t desc = c->tag(0x80000007).value_or(0);
        REQUIRE((desc == 3 || desc == 32));
        const vt::Anim an = vt::readAnim(c->bytes, *firstForm(*c, kAnim));
        const vt::DveInfo info = vt::dveInfo(an);
        auto e = load(src);
        REQUIRE(e);
        const std::vector<vt::Rgb>* palette = &an.bm.cmap;
        for (int k = 0; k < e->frameCount(); ++k) {
            if (k >= 1 && size_t(k) < an.cmaps.size() && an.cmaps[size_t(k)] && !an.cmaps[size_t(k)]->empty())
                palette = &*an.cmaps[size_t(k)];
            const auto routes = vt::dveRoutes(c->blobs.at(kTagSourceWords), *palette);
            const vt::DveControl dc = vt::decodeDve(an.planes[size_t(k)]);
            vt::RenderCtx ctx = ctxWith(kWhite);
            ctx.animFrame = k;
            ctx.digitalA = &d;
            vt::Frame got;
            e->render(0, a, b, got, ctx);
            // the DVE picture: A's DV picture moved over the background, before routes and art
            const vt::Frame* moving = &d;
            const vt::Frame* background = &b;
            switch (info.role) {
                case vt::DveRole::MovingA: break;
                case vt::DveRole::MovingB: moving = &b; background = &a; break;
                case vt::DveRole::DistortA: background = &a; break;
                case vt::DveRole::Flip:
                    moving = k < info.switchFrame ? &d : &b;
                    background = &black;
                    break;
            }
            vt::DveControl noArt = dc;
            noArt.art.clear();
            vt::Frame pic;
            vt::renderDve(noArt, *moving, *background, {}, pic, ctx.guesses);
            for (int y = 0; y < vt::kRasterH; ++y)
                for (int x = 0; x < vt::kRasterW; ++x) {
                    const uint8_t idx = dc.index[size_t(y / 2) * 768 + size_t(x)];
                    if (idx >= routes.size() || routes[idx].level != 0) continue;
                    if (routes[idx].source == 1) {
                        ++s1;
                        if (!same(got.at(x, y), b.at(x, y))) FAIL("frame " << k << " S = 1 pixel (" << x << "," << y << ") is not B");
                    } else if (routes[idx].source == 3) {
                        ++s3;
                        if (!same(got.at(x, y), pic.at(x, y))) FAIL("frame " << k << " S = 3 pixel (" << x << "," << y << ") is not the moved DV picture");
                    }
                }
        }
    }
    INFO("S = 1 pixels " << s1 << ", S = 3 pixels " << s3);
    REQUIRE(s1 > 0);
    REQUIRE(s3 > 0);
}

// Fly Over Crumpled Edge !'s foil edge takes its colour from the whole display index: plane 2 is
// part of it (index 60 is dark, 19 27 31; the old cmap[group * 8] drew cmap[56], 217 235 255).
TEST_CASE("Fly Over Crumpled Edge ! draws its edge in the display index's palette colour", "[dve][effect]") {
    const std::string src = "4.2/Effects/4000FX/Digital/Fly Over Crumpled Edge !";
    const auto c = vt::loadCrouton(golden::sourcePath(src));
    REQUIRE(c);
    const vt::Anim an = vt::readAnim(c->bytes, *firstForm(*c, kAnim));
    auto e = load(src);
    REQUIRE(e);
    const vt::Frame a = ramp(1), b = ramp(2);
    vt::RenderCtx ctx = ctxWith(kWhite);
    ctx.animFrame = 58;
    vt::Frame got;
    e->render(0, a, b, got, ctx);
    const vt::DveControl dc = vt::decodeDve(an.planes[58]);
    const auto& cmap = an.bm.cmap;
    int edge = 0;
    for (int y = 0; y < vt::kRasterH; ++y)
        for (int x = 0; x < vt::kRasterW; ++x) {
            const size_t at = size_t(y / 2) * 768 + size_t(x);
            if (dc.art[at] == 0 || dc.key[size_t(y / 2) * 192 + size_t(x / 4)] >= 64) continue;
            const vt::Rgb p = cmap[dc.index[at] & 0xFC];
            ++edge;
            if (!same(got.at(x, y), {p.r, p.g, p.b, 255})) FAIL("edge pixel (" << x << "," << y << ") is not its palette colour");
        }
    REQUIRE(edge >= 10000);
    REQUIRE(same(got.at(384, 110), {cmap[60].r, cmap[60].g, cmap[60].b, 255}));
}

// Warp To Globe !'s S = 9 indexes hold a shaded Earth (palette colours with non-zero high
// nibbles): the warped picture dissolves into it and it flies off. Before, those pixels showed the
// generator colour, a flat disc.
TEST_CASE("Warp To Globe ! shows its Earth, not the generator colour", "[dve][effect]") {
    const std::string src = "4.2/Effects/4000FX/Digital/Warp To Globe !";
    const auto c = vt::loadCrouton(golden::sourcePath(src));
    REQUIRE(c);
    const vt::Anim an = vt::readAnim(c->bytes, *firstForm(*c, kAnim));
    const auto routes = vt::dveRoutes(c->blobs.at(kTagSourceWords), an.bm.cmap);
    auto e = load(src);
    REQUIRE(e);
    const vt::Frame a = ramp(1), b = ramp(2);
    vt::RenderCtx ctx = ctxWith(kWhite);
    ctx.animFrame = 45;
    vt::Frame got;
    e->render(0, a, b, got, ctx);
    const vt::DveControl dc = vt::decodeDve(an.planes[45]);
    int earth = 0;
    for (int y = 0; y < vt::kRasterH; ++y)
        for (int x = 0; x < vt::kRasterW; ++x) {
            const uint8_t i = dc.index[size_t(y / 2) * 768 + size_t(x)];
            if (i >= routes.size() || routes[i].source != 9 || routes[i].level != 0) continue;
            const vt::Rgb p = an.bm.cmap[i];
            if (((p.r | p.g | p.b) & 0xF0) == 0) continue;
            ++earth;
            if (!same(got.at(x, y), {p.r, p.g, p.b, 255})) FAIL("Earth pixel (" << x << "," << y << ") is not its palette colour");
        }
    REQUIRE(earth >= 5000);
}

// Infinite Video Space ! has no tag 0x1B: its palette's own level bits apply. Frame 0's art is
// black at L = 3 over the whole picture, so the transition opens on A, not black; its grey tiles
// (L = 0, over the background) keep their palette colour.
TEST_CASE("Infinite Video Space ! opens on A and keeps its tiles", "[dve][effect]") {
    const std::string src = "4.2/Effects/4000FX/Space/Infinite Video Space !";
    auto e = load(src);
    REQUIRE(e);
    const vt::Frame a = ramp(1), b = ramp(2);
    vt::RenderCtx ctx = ctxWith(kWhite);
    ctx.animFrame = 0;
    vt::Frame got;
    e->render(0, a, b, got, ctx);
    const int x = vt::kVisibleArea.x + vt::kVisibleArea.w / 2, y = vt::kVisibleArea.y + vt::kVisibleArea.h / 2;
    REQUIRE(same(got.at(x, y), a.at(x, y)));

    const auto c = vt::loadCrouton(golden::sourcePath(src));
    REQUIRE(c);
    const vt::Anim an = vt::readAnim(c->bytes, *firstForm(*c, kAnim));
    ctx.animFrame = 22;
    e->render(0, a, b, got, ctx);
    const vt::DveControl dc = vt::decodeDve(an.planes[22]);
    int tiles = 0;
    for (int yy = 0; yy < vt::kRasterH; ++yy)
        for (int xx = 0; xx < vt::kRasterW; ++xx) {
            const size_t at = size_t(yy / 2) * 768 + size_t(xx);
            const uint8_t ci = dc.index[at] & 0xFC;
            if (dc.art[at] == 0 || ci < 16 || (an.bm.cmap[ci].b & 3) != 0) continue;
            const vt::Rgb p = an.bm.cmap[ci];
            ++tiles;
            if (!same(got.at(xx, yy), {p.r, p.g, p.b, 255})) FAIL("tile pixel (" << xx << "," << yy << ") is not its palette colour");
        }
    REQUIRE(tiles >= 1000);
}

TEST_CASE("the seven \"!\" DVEs render without unknown codes", "[dve][effect]") {
    const vt::Frame a = solid({10, 20, 30, 255}), b = solid({40, 50, 60, 255});
    const vt::Rect r = vt::kVisibleArea;
    for (const char* src : kBangDves) {
        INFO(src);
        auto e = load(src);
        REQUIRE(e);
        for (int k = 0; k < e->frameCount(); ++k) {
            vt::RenderCtx ctx = ctxWith(kWhite);
            ctx.animFrame = k;
            vt::Frame out;
            e->render(0, a, b, out, ctx);
            for (int y = r.y; y < r.y + r.h; ++y)
                for (int x = r.x; x < r.x + r.w; ++x)
                    if (same(out.at(x, y), {128, 128, 128, 255})) FAIL("frame " << k << " grey at (" << x << "," << y << ")");
        }
    }
}

// --- digital moves (FREEZE.md 5) ---

TEST_CASE("digitalMove by role", "[dve][algo][effect]") {
    for (auto [src, role, want] : {std::tuple{"4.2/Effects/2000FX/Blinds/Split Corners In", vt::DveRole::MovingB, vt::DigitalMove::B},
                                   std::tuple{"4.2/Effects/2000FX/Digital/Block Out", vt::DveRole::MovingA, vt::DigitalMove::A}}) {
        INFO(src);
        const AnimFile f = openAnim(src);
        REQUIRE(vt::dveInfo(f.anim).role == role);
        const auto e = load(src);
        REQUIRE(e);
        CHECK(e->digitalMove() == want);
    }
    for (auto [name, desc, want] : {std::tuple{"Push On L-R", 2u, vt::DigitalMove::B},
                                    std::tuple{"Crop To Center", 3u, vt::DigitalMove::A}}) {
        INFO(name);
        const auto c = vt::loadCrouton(algoNamed(name));
        REQUIRE(c);
        REQUIRE(c->tag(0x80000063).value_or(0) == 0);  // kind 0: digital
        REQUIRE(c->tag(0x80000007).value_or(0) == desc);
        CHECK(loadAlgo(name)->digitalMove() == want);
    }
    CHECK(vt::makeDissolve()->digitalMove() == vt::DigitalMove::None);
    CHECK(vt::makeCut()->digitalMove() == vt::DigitalMove::None);
    CHECK(load("4.2/Effects/ChromaFx/Fade To Neg")->digitalMove() == vt::DigitalMove::None);
}

// The moving clip comes from RenderCtx::digitalA / digitalB when given; the background stays a / b:
// rendering (a, b) with the moving bus's digital picture d equals rendering with d on that bus.
TEST_CASE("digital moves move the DV picture over the analog background", "[dve][algo][effect]") {
    const vt::Frame a = pattern(1), b = pattern(2), d = pattern(3);
    auto check = [&](const vt::Effect& e, int tbar) {
        const bool movesB = e.digitalMove() == vt::DigitalMove::B;
        vt::RenderCtx c = ctxWith();
        (movesB ? c.digitalB : c.digitalA) = &d;
        c.take = 1;
        const vt::Frame got = renderAt(e, tbar, a, b, c);
        vt::RenderCtx plain = ctxWith();
        plain.take = 2;
        CHECK(got.px == renderAt(e, tbar, movesB ? a : d, movesB ? d : b, plain).px);
        plain.take = 3;
        CHECK(got.px != renderAt(e, tbar, a, b, plain).px);
    };
    for (const char* src : {"4.2/Effects/2000FX/Blinds/Split Corners In", "4.2/Effects/2000FX/Digital/Block Out"}) {
        INFO(src);
        const auto e = load(src);
        REQUIRE(e);
        check(*e, 200);
    }
    for (const char* name : {"Push On L-R", "Crop To Center"}) {
        INFO(name);
        check(*loadAlgo(name), 300);
    }
}
