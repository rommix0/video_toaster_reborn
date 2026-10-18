#include <algorithm>
#include <array>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "catch_amalgamated.hpp"
#include "golden.h"
#include "vt/anim.h"
#include "vt/background.h"
#include "vt/crouton.h"
#include "vt/effect.h"
#include "vt/ilbm.h"
#include "vt/mix.h"
#include "vt/sound.h"
#include "vt/timing.h"

namespace {

constexpr std::array<char, 4> kAnim{'A', 'N', 'I', 'M'};
constexpr std::array<char, 4> kIlbm{'I', 'L', 'B', 'M'};
constexpr uint32_t kTagMode = 0x80000022;
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

// Gradient model: B where gray >= 255 - tbar * 255 / 511; black where the raster line has no
// control line.
void requireGradient(const vt::Effect& e, const Gradient& g, int tbar, int softness = 0) {
    const Ctl ctl{g.img.w, g.img.h};
    const vt::Frame a = solid(kA), b = solid(kB);
    vt::RenderCtx c = ctxWith();
    c.guesses.gradientSoftness = softness;
    const vt::Frame out = renderAt(e, tbar, a, b, c);
    const int s = softness;
    const int thr = (255 + s) - tbar * (255 + s) / 511;
    requireFrame(out, [&](int x, int y) {
        const int line = ctl.line(y);
        if (line < 0) return kBlack;
        const int gray = g.gray[g.img.px[size_t(line) * size_t(g.img.w) + size_t(ctl.x(x))]];
        int level;
        if (s == 0) level = gray >= thr ? 255 : 0;
        else level = std::clamp(255 * (gray - (thr - s)) / s, 0, 255);
        // a negative numerator must not round toward zero into a partial level
        if (s > 0 && gray - (thr - s) < 0) level = 0;
        return lerpPx(kA, kB, level);
    });
}

}  // namespace

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
    REQUIRE(renderAt(*e, 0, a, b, c).px == a.px);
    REQUIRE(renderAt(*e, 511, a, b, c).px == b.px);
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
// palette does not change).
vt::Rgba8 goldenPixel(const golden::Record& r, int idx, int frame, int tbar, vt::Rgba8 pa = kA,
                      vt::Rgba8 pb = kB) {
    const std::string& kind = str(r, "kind");
    const auto& over_rgba = arr(r, "over_rgba");
    int level;
    if (kind == "overlay") {
        const int peak = int(num(r, "flash_peak"));
        level = peak >= 0 ? (frame >= peak ? 255 : 0) : (tbar >= 256 ? 255 : 0);
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
        return goldenPixel(rec, idx, frame, tbar);
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
        const vt::Rgba8 video = pass == 0 ? kA : kB;
        const vt::Frame out = renderAt(*e, tbar, solid(kA), solid(kB));
        size_t overlayPixels = 0;
        requireFrame(out, [&](int x, int y) {
            const int idx = f.px[size_t(y / 2) * size_t(f.w) + size_t(x)];
            if (over_rgba[size_t(idx) * 4 + 3] != 0) {
                ++overlayPixels;
                return vt::Rgba8{uint8_t(over_rgba[size_t(idx) * 4]), uint8_t(over_rgba[size_t(idx) * 4 + 1]),
                                 uint8_t(over_rgba[size_t(idx) * 4 + 2]), 255};
            }
            // the frame holds only the video index of its own end (and overlay)
            REQUIRE(idx == (pass == 0 ? aIdx : bIdx));
            return video;
        });
        (void)overlayPixels;
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
    bool neighbourDiffers = false;
    for (int frame : {n / 4, n / 2, n - 1}) {
        const auto& f = af.anim.frames[size_t(frame)];
        const int tbar = tbarForFrame(frame, n);
        const vt::Frame out = renderAt(*e, tbar, black, white);
        auto ctrl = [&](int cx, int cy) {
            return goldenPixel(rec, f.px[size_t(cy) * 384 + size_t(cx)], frame, tbar, kBlack, kWhite);
        };
        // control pixel (10, 5) covers exactly raster (20..21, 10..11)
        for (int y = 10; y <= 11; ++y)
            for (int x = 20; x <= 21; ++x) REQUIRE(same(out.at(x, y), ctrl(10, 5)));
        // and the whole picture follows the same rule, control line 240 on lines 480-481
        for (int y = 0; y < vt::kRasterH; ++y)
            for (int x = 0; x < vt::kRasterW; ++x)
                if (!same(out.at(x, y), ctrl(x / 2, y / 2))) FAIL("mismatch at " << x << "," << y);
        if (!same(ctrl(10, 5), ctrl(11, 5)) || !same(ctrl(10, 5), ctrl(10, 6))) neighbourDiffers = true;
    }
    (void)neighbourDiffers;
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
        if (af.plan.kind == vt::AnimKind::Overlay) level = peak >= 0 ? (frame >= peak ? 255 : 0) : (tbar >= 256 ? 255 : 0);
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

TEST_CASE("loadEffect refuses what it cannot play, with a reason", "[effect][factory]") {
    std::string why;
    // DVE (Ruling 2)
    int dve = 0;
    for (const auto& r : golden::readLines("dve.jsonl")) {
        why.clear();
        INFO(str(r, "source"));
        REQUIRE(load(str(r, "source"), &why) == nullptr);
        REQUIRE(why == "DVE support not built yet");
        ++dve;
    }
    REQUIRE(dve == 64);
    // ChromaFX
    why.clear();
    REQUIRE(load("4.2/Effects/ChromaFx/BW Poster", &why) == nullptr);
    REQUIRE(why == "ChromaFX support not built yet");
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

TEST_CASE("ALGO, MAIN and CTRL croutons are Switcher moves", "[effect][factory]") {
    std::map<vt::CroutonType, int> seen;
    const char* names[] = {"ALGO", "MAIN", "CTRL"};
    for (auto& ent : std::filesystem::recursive_directory_iterator(dataRoot() / "4.2" / "Effects")) {
        if (!ent.is_regular_file()) continue;
        auto ext = ent.path().extension().string();
        if (ext == ".i" || ext == ".I") continue;
        auto c = vt::loadCrouton(ent.path());
        if (!c) continue;
        int which = -1;
        if (c->type == vt::CroutonType::Algo) which = 0;
        else if (c->type == vt::CroutonType::Main) which = 1;
        else if (c->type == vt::CroutonType::Ctrl) which = 2;
        if (which < 0) continue;
        ++seen[c->type];
        std::string why;
        INFO(ent.path().string());
        REQUIRE(vt::loadEffect(ent.path(), &why) == nullptr);
        REQUIRE(why == std::string("Switcher move (") + names[which] + ") not reverse-engineered yet");
    }
    REQUIRE(!seen.empty());
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
        for (int t : {0, 300, 511}) requireGradient(*e, g, t);
    }
    INFO("refused reasons: " << (reasons.empty() ? "none" : *reasons.begin()));
    REQUIRE(loaded > 350);
    REQUIRE(sounds >= 1);
    // everything refused among the animated wipes has a known reason
    for (const auto& why : reasons) REQUIRE(why == "overlay effect with nothing but video in it");
}
