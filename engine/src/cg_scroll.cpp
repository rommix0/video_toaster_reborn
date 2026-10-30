#include "vt/cg_scroll.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <exception>
#include <initializer_list>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "log_detail.h"
#include "vt/crouton.h"
#include "vt/ilbm.h"

// A port of ../tools/re/cgscroll_ref.py (CGSCROLL.md): Page (the parser), render_line / fed_rows (the
// scroll line, switcher.main 0x1342ee), crawl_glyph / crawl_windows (0x133c6a / 0x134de4), palette,
// scroll_run (0x1011e0 + the interrupt 0x101bb8) and the crawl's field law. Where the model's numpy
// slicing clips, the loops here clip the same way.
namespace vt {
namespace {

struct Refusal : std::runtime_error {
    using std::runtime_error::runtime_error;
};

void checkSize(const std::vector<uint8_t>& px) {
    if (px.size() > kCgScrollMaxBytes) throw Refusal("the page is too large (its rows pass 64 MB)");
}

constexpr int kDx[8] = {0, -1, -1, -1, 0, 1, 1, 1};  // 0x10ce1c, by attribute bits 5-7
constexpr int kDy[8] = {1, 1, 0, -1, -1, -1, 0, 1};  // 0x10ce2c
struct Outline {
    int cx, cy, ex, ey;  // brush centre and extent (0x10ce3c)
};
constexpr Outline kOutline[4] = {{0, 0, 0, 0}, {4, 2, 8, 4}, {4, 2, 8, 4}, {8, 4, 16, 8}};

struct Pt {
    int x, y;
};
std::vector<Pt> brushRows(std::initializer_list<unsigned> rows, int w) {
    std::vector<Pt> out;
    int y = 0;
    for (unsigned r : rows) {
        for (int x = 0; x < w; ++x)
            if (r & (1u << (w - 1 - x))) out.push_back({x, y});
        ++y;
    }
    return out;
}
// The outline rings (0x10604e / 0x106056 / 0x10605e): points from the brush origin.
const std::vector<Pt>& brush(int t) {
    static const std::vector<Pt> none;
    static const std::vector<Pt> b1{{4, 0}, {3, 1}, {5, 1}, {2, 2}, {6, 2}, {3, 3}, {5, 3}, {4, 4}};
    static const std::vector<Pt> b2 = brushRows({0x3e, 0x63, 0x63, 0x63, 0x3e}, 8);
    static const std::vector<Pt> b3 =
        brushRows({0x03e0, 0x1c1c, 0x2002, 0x4001, 0x4001, 0x4001, 0x2002, 0x1c1c, 0x03e0}, 16);
    return t == 1 ? b1 : t == 2 ? b2 : t == 3 ? b3 : none;
}

struct Attr {
    int outline, shadow, order, dir, len;
    bool crawl;
};
Attr attrOf(uint32_t a) {
    return {int(a & 3), int(a >> 2 & 3), int(a >> 4 & 1), int(a >> 5 & 7), int(a >> 8 & 15), (a >> 31) != 0};
}

// One byte per pixel, row-major.
struct Plane {
    int w = 0, h = 0;
    std::vector<uint8_t> px;
    Plane(int w_, int h_) : w(w_), h(h_), px(size_t(w_) * size_t(h_)) {}
    uint8_t& at(int x, int y) { return px[size_t(y) * size_t(w) + size_t(x)]; }
    uint8_t at(int x, int y) const { return px[size_t(y) * size_t(w) + size_t(x)]; }
};

// Rows [y0, y1] x columns [x0, x1] (inclusive) clipped to the plane; empty when x1 < x0 or y1 < y0.
struct Box {
    int x0, y0, x1, y1;
};
Box clip(Box b, const Plane& p) {
    return {std::max(b.x0, 0), std::max(b.y0, 0), std::min(b.x1, p.w - 1), std::min(b.y1, p.h - 1)};
}

// dst |= shift(src restricted to `in`, dx, dy) with zero fill (cgscroll_ref.shift). `bit(x, y)` reads src.
template <class Bit>
void orShifted(Plane& dst, Box in, Bit bit, int dx, int dy) {
    for (int y = in.y0; y <= in.y1; ++y) {
        const int ty = y + dy;
        if (ty < 0 || ty >= dst.h) continue;
        for (int x = in.x0; x <= in.x1; ++x) {
            const int tx = x + dx;
            if (tx >= 0 && tx < dst.w && bit(x, y)) dst.at(tx, ty) = 1;
        }
    }
}

uint16_t be16(std::span<const uint8_t> b, size_t o) { return uint16_t(b[o] << 8 | b[o + 1]); }
uint32_t be32(std::span<const uint8_t> b, size_t o) { return uint32_t(be16(b, o)) << 16 | be16(b, o + 2); }

struct Glyph {
    int x, y, w, h;  // in the strip
};
struct Placement {
    int glyph, x;  // x: the column on a scroll line; a crawl's signed gap
    uint32_t attr;
};

// The file: the first FORM ILBM (the coverage strip) and FORM MVPG (CGSCROLL.md section 1).
struct Page {
    IndexImage strip;
    int maxh = 0;
    int mvpgSpeed = 0;
    std::array<uint8_t, 3> face{}, edge{};
    std::vector<Glyph> glyphs;
    std::vector<Placement> placements;
    std::vector<std::pair<int, int>> lines;  // placement count, y
    std::vector<size_t> firstPlacement;      // per line
};

Page readPage(const Crouton& c) {
    std::span<const uint8_t> d(c.bytes);
    auto first = [&](std::string_view type) {
        const auto f = std::find_if(c.forms.begin(), c.forms.end(),
                                    [&](const Form& x) { return std::string_view(x.type.data(), 4) == type; });
        if (f == c.forms.end()) throw Refusal("no FORM " + std::string(type));
        return *f;
    };
    Page p;
    const auto [bm, strip] = readIlbm(d, first("ILBM"));
    if (bm.planes > 2) throw Refusal("the glyph strip has " + std::to_string(bm.planes) + " planes (want 2)");
    p.strip = strip;

    const Form m = first("MVPG");
    if (m.offset + 8 + m.size > d.size() || m.size < 4 + 0x34) throw Refusal("FORM MVPG cut off");
    const auto b = d.subspan(m.offset + 12, m.size - 4);
    const uint32_t nplace = be32(b, 0), nglyph = be32(b, 4), nline = be32(b, 8);
    p.maxh = be16(b, 0x0e);
    p.mvpgSpeed = b[0x15];
    std::copy_n(b.begin() + 0x18, 3, p.face.begin());
    std::copy_n(b.begin() + 0x1c, 3, p.edge.begin());
    const uint64_t need = 0x34 + 12ull * nglyph + 8ull * nplace + 4ull * nline;
    if (need > b.size()) throw Refusal("MVPG records run past the form");
    size_t o = 0x34;
    for (uint32_t i = 0; i < nglyph; ++i, o += 12) {
        const Glyph g{be16(b, o + 2), be16(b, o + 4), be16(b, o + 6), be16(b, o + 8)};
        if (g.x + g.w > p.strip.w || g.y + g.h > p.strip.h) throw Refusal("glyph " + std::to_string(i) + " runs past the strip");
        p.glyphs.push_back(g);
    }
    for (uint32_t i = 0; i < nplace; ++i, o += 8) {
        const int g = be16(b, o);
        if (g >= int(nglyph)) throw Refusal("placement " + std::to_string(i) + " names no glyph");
        p.placements.push_back({g, int16_t(be16(b, o + 2)), be32(b, o + 4)});
    }
    size_t k = 0;
    for (uint32_t i = 0; i < nline; ++i, o += 4) {
        p.lines.emplace_back(be16(b, o), be16(b, o + 2));
        p.firstPlacement.push_back(k);
        k += be16(b, o);
    }
    if (k > nplace) throw Refusal("the lines name more placements than there are");
    if (p.lines.empty() || p.placements.empty()) throw Refusal("an empty page");
    return p;
}

// cgscroll_ref.palette, as RGB: 12-bit (high nibbles) widened by nibble * 17.
std::array<Rgba8, 8> paletteOf(const Page& p) {
    std::array<int, 3> c1{}, c2{};
    for (int i = 0; i < 3; ++i) {
        c1[size_t(i)] = (p.edge[size_t(i)] * 0xaaaa + p.face[size_t(i)] * 0x5555) >> 16;
        c2[size_t(i)] = ((p.edge[size_t(i)] * 0xaaaa >> 1) + p.face[size_t(i)] * 0xaaaa) >> 16;
    }
    auto rgb = [](auto c) { return Rgba8{uint8_t((c[0] >> 4) * 17), uint8_t((c[1] >> 4) * 17), uint8_t((c[2] >> 4) * 17), 255}; };
    const Rgba8 face = rgb(p.face);
    return {Rgba8{0, 0, 0, 255}, face, face, face, rgb(p.edge), rgb(c1), rgb(c2), face};
}

// ---- scroll ----

constexpr int kLineW = 640, kLineH = 480, kRing = 500;

int hmaxOf(const Page& p, int line) {
    int h = 0;
    for (int k = 0; k < p.lines[size_t(line)].first; ++k)
        h = std::max(h, p.glyphs[size_t(p.placements[p.firstPlacement[size_t(line)] + size_t(k)].glyph)].h);
    return h;
}

// fed_rows: the line's tallest glyph + 4, or the page's max height + 4 for an empty line.
int fedRows(const Page& p, int line) {
    const int h = hmaxOf(p, line);
    return (h ? h : p.maxh) + 4;
}

// render_line: the line's Amiga indices (coverage | plane 2 << 2), 640 x 480.
std::vector<uint8_t> renderLine(const Page& p, int line) {
    Plane cov(kLineW, kLineH), sh(kLineW, kLineH);
    const int count = p.lines[size_t(line)].first;
    const size_t k0 = p.firstPlacement[size_t(line)];
    if (count > 0) {
        const Attr f = attrOf(p.placements[k0].attr);  // the first placement's attributes hold for the line
        const int t = f.outline, s = f.shadow, L = f.len, d = f.dir;
        const int dyl = kDy[d] * L;  // 0x133fff: taken even when the shadow type is 0
        const int y0 = std::clamp((dyl < 0 ? -dyl : 0) + kOutline[t].cy + 1, 0, 8);
        const int hmax = hmaxOf(p, line);
        for (int k = 0; k < count; ++k) {
            const Placement& pl = p.placements[k0 + size_t(k)];
            const Glyph& g = p.glyphs[size_t(pl.glyph)];
            const int y = y0 + (hmax > g.h ? hmax - g.h - 8 : 0);
            for (int r = std::max(0, -y); r < g.h && y + r < kLineH; ++r)
                for (int c = std::max(0, -pl.x); c < g.w && pl.x + c < kLineW; ++c)
                    cov.at(pl.x + c, y + r) |= p.strip.px[size_t(g.y + r) * size_t(p.strip.w) + size_t(g.x + c)];
        }
        if (t || s) {
            // The rows holding any coverage: every source below is empty outside them.
            int top = kLineH, bottom = -1;
            for (int y = 0; y < kLineH; ++y)
                if (std::any_of(cov.px.begin() + y * kLineW, cov.px.begin() + (y + 1) * kLineW, [](uint8_t v) { return v; })) {
                    top = std::min(top, y);
                    bottom = y;
                }
            const Box rows{0, top, kLineW - 1, bottom};
            auto m = [&](int x, int y) { return cov.at(x, y) != 0; };
            Plane o(kLineW, kLineH);
            if (!t) orShifted(o, rows, m, 0, 0);
            int cx = kOutline[t].cx, cy = kOutline[t].cy;
            if (f.order && s) {  // GUESS (the model's): order bit 1 moves the brush origin by the shadow
                cx -= std::min(0, kDx[d] * L);
                cy -= std::min(0, kDy[d] * L);
            }
            for (const Pt& b : brush(t)) orShifted(o, rows, m, b.x - cx, b.y - cy);
            if (s) orShifted(sh, rows, m, 0, 0);  // 0x1343d4: the face mask first
            const int pad = 16 + 15;               // the brush and the shadow reach this far
            const Box oRows{0, std::max(0, top - pad), kLineW - 1, std::min(kLineH - 1, bottom + pad)};
            auto ob = [&](int x, int y) { return o.at(x, y) != 0; };
            if (s == 1) {
                for (int k = 0; k < L; ++k) orShifted(sh, oRows, ob, k * kDx[d], k * kDy[d]);
            } else if (s) {
                orShifted(sh, oRows, ob, 0, 0);
                orShifted(sh, oRows, ob, L * kDx[d], L * kDy[d]);
            } else {
                orShifted(sh, oRows, ob, 0, 0);
            }
        }
    }
    for (size_t i = 0; i < cov.px.size(); ++i) cov.px[i] = uint8_t(cov.px[i] | sh.px[i] << 2);
    return std::move(cov.px);
}

// scroll_run + scroll_stream: the rows fed in order and the rows fed after every field.
void runScroll(const Page& p, CgScroll& out) {
    constexpr int kMaxFields = 1 << 20;
    const bool slow = out.speed == 0;
    int T = 0, feed = 0, src = 0, field = 0;
    bool lof = false;  // the first field is a short one
    std::vector<uint8_t> line;
    auto tick = [&] {
        const int inc = slow ? (lof ? 1 : 0) : out.speed;
        const int n = std::min(inc, feed);
        for (int r = 0; r < n; ++r, ++src) {
            if (src < kLineH) out.px.insert(out.px.end(), line.begin() + src * kLineW, line.begin() + (src + 1) * kLineW);
            else out.px.resize(out.px.size() + kLineW);  // past the line buffer (an empty line taller than it)
        }
        out.px.resize(out.px.size() + size_t(inc - n) * kLineW);
        checkSize(out.px);
        feed -= n;
        T += inc;
        out.tops.push_back(T);
        lof = !lof;
        if (++field > kMaxFields) throw Refusal("the scroll does not end");
    };
    for (int i = 0; i < int(p.lines.size()); ++i) {
        while (feed) tick();                           // 0x1012b6: the previous line fully fed
        while (p.lines[size_t(i)].second > T) tick();  // 0x1012de: until $32 >= the line's y
        line = renderLine(p, i);
        src = 0;
        feed = fedRows(p, i);
    }
    while (feed) tick();
    out.rows = int(out.px.size() / kLineW);
    if (out.pause) {  // 0x1021f6: stop, hold, fade 22 fields
        out.stopField = field;
        out.endField = field + out.hold + 22;
    } else {  // 0x101398: 500 more rows, then off
        for (const int goal = T + kRing; goal > T;) tick();
        out.endField = field;
        out.px.resize(size_t(out.rows) * kLineW);  // rows past the stream are 0
    }
}

// ---- crawl ----

constexpr int kCrawlW = 1536, kCrawlRows = 300;

// crawl_glyph: draws placement k at (x, y) into the window buffers; returns the advance.
int crawlGlyph(const Page& p, Plane& cov, Plane& sh, size_t k, int x, int y) {
    const Placement& pl = p.placements[k];
    const Glyph& g = p.glyphs[size_t(pl.glyph)];
    const int x0 = std::max(0, x), x1 = std::min(cov.w, x + g.w);
    for (int r = 0; r < g.h && y + r < cov.h; ++r)
        for (int c = x0; c < x1; ++c) cov.at(c, y + r) |= p.strip.px[size_t(g.y + r) * size_t(p.strip.w) + size_t(g.x + c - x)];
    const Attr f = attrOf(pl.attr);
    const int t = f.outline, s = f.shadow, L = f.len, d = f.dir;
    if (t || s) {
        // Outline and shadow both come from coverage plane 1 (0x133e0c..0x133f20), not the outline.
        auto p1 = [&](int cx, int cy) { return (cov.at(cx, cy) >> 1 & 1) != 0; };
        const Outline ol = kOutline[t];
        const int rx0 = x, ry0 = y, rx1 = x + g.w - 1, ry1 = std::min(y + g.h - 1, kCrawlRows);
        const int cx0 = rx0 - ol.cx, cy0 = ry0 - ol.cy;
        const int cx1 = rx1 + ol.ex - ol.cx, cy1 = ry1 + ol.ey - ol.cy + 4;
        const Box glyph = clip({std::max(0, rx0), ry0, rx1, ry1}, cov);
        if (t) {
            for (const Pt& b : brush(t)) orShifted(sh, glyph, p1, b.x - ol.cx, b.y - ol.cy);
        } else {
            for (int r = glyph.y0; r <= glyph.y1; ++r)
                for (int c = glyph.x0; c <= glyph.x1; ++c) sh.at(c, r) = p1(c, r);
        }
        if (s) {
            const Box src = clip({std::max(0, cx0), std::max(0, cy0), cx1 + 12, cy1 + 8 + 20}, cov);
            if (s == 1) {
                for (int j = 0; j < L; ++j) orShifted(sh, src, p1, j * kDx[d], j * kDy[d]);
            } else {
                orShifted(sh, src, p1, 0, 0);
                orShifted(sh, src, p1, L * kDx[d], L * kDy[d]);
            }
        } else {  // 0x133f7c: minterm 0x60 (XOR): the face is cut out again
            const Box b = clip({std::max(0, cx0), std::max(0, cy0), cx1, cy1}, cov);
            for (int r = b.y0; r <= b.y1; ++r)
                for (int c = b.x0; c <= b.x1; ++c) sh.at(c, r) ^= uint8_t(p1(c, r));
        }
    }
    const int adv = f.crawl ? (g.w + pl.x) & 0xffff : g.w;
    return adv ? adv : 1;
}

// crawl_windows + crawl_strip: the continuous strip's band rows, window by window.
void runCrawl(const Page& p, CgScroll& out) {
    constexpr int kMaxWindows = 4096;
    const Attr f0 = attrOf(p.placements[0].attr);
    const int ty = (f0.shadow ? std::min(0, kDy[f0.dir]) : 0) - kOutline[f0.outline].cy;  // crawl_y
    const int y = ty > 0 ? 0 : -ty + 12;
    out.rows = (std::min(p.maxh + 0x1e, kCrawlRows) >> 1) * 2;
    out.bandTop = 2 * (p.lines[0].second >> 1) + 44;
    const size_t nplace = p.placements.size();
    const size_t window = size_t(out.rows) * 768;  // one window's band rows x columns 768n..
    int n = 0;
    Plane cov(kCrawlW, kCrawlRows), sh(kCrawlW, kCrawlRows);
    size_t k = 0;
    int64_t X = 0;
    int cd = 0;
    for (;;) {
        std::fill(cov.px.begin(), cov.px.end(), 0);
        std::fill(sh.px.begin(), sh.px.end(), 0);
        if (k == 0) X = 0x2ee;  // 750 - rect x0 (0)
        constexpr int64_t kLimit = 0x5e5;  // 1509 - rect x1 (0)
        int64_t d5 = X, d3 = 1;
        bool drew = false;
        if (k < nplace) {
            while (d5 <= kLimit && d3) {
                if (!cd) {
                    d3 = k < nplace ? crawlGlyph(p, cov, sh, k, int(d5), y) : 0;
                    if (d3) ++k;
                    drew = d3 != 0;
                }
                d5 += d3;
            }
        }
        if (k) --k;  // the glyph that crosses 1509 is drawn again in the next window
        X = d5 > kLimit ? d5 - d3 - kLimit + 0x2e6 : d5 - d3;
        out.px.resize(out.px.size() + window);
        checkSize(out.px);
        uint8_t* w = out.px.data() + size_t(n++) * window;
        for (int r = 0; r < out.rows; ++r)
            for (int c = 0; c < 768; ++c) w[size_t(r) * 768 + size_t(c)] = uint8_t(cov.at(742 + c, r) | sh.at(742 + c, r) << 2);
        if (!drew && !cd) cd = 4;
        if (cd && --cd == 1) break;
        if (n > kMaxWindows) throw Refusal("the crawl does not end");
    }
    out.width = 768 * n;
    out.pxPerField = 2 << std::min(out.speed, 3);
    out.endField = 768 * (n - 1) / out.pxPerField;  // the display swaps to the last window
}

}  // namespace

bool isCgScroll(const std::filesystem::path& path) {
    try {
        const auto c = loadCrouton(path);
        return c && (c->type == CroutonType::Scro || c->type == CroutonType::Craw);
    } catch (const std::exception&) {
        return false;
    }
}

std::optional<CgScroll> loadCgScroll(const std::filesystem::path& path, std::string* whyNot, int speed) {
    try {
        const auto c = loadCrouton(path);
        if (!c) throw Refusal("cannot read " + detail::utf8(path));
        if (c->type != CroutonType::Scro && c->type != CroutonType::Craw) throw Refusal("not a scroll or crawl page (TYPE SCRO / CRAW)");
        const Page p = readPage(*c);
        CgScroll out;
        out.path = path;
        out.kind = c->type == CroutonType::Scro ? CgScroll::Kind::Scroll : CgScroll::Kind::Crawl;
        out.speed = speed >= 0 ? speed : int(std::min<uint32_t>(c->tag(0x8000006a).value_or(uint32_t(p.mvpgSpeed)), 4));
        out.palette = paletteOf(p);
        if (out.kind == CgScroll::Kind::Scroll) {
            out.speed = std::min(out.speed, 4);
            const uint32_t pauseTag = c->tag(0x80000087).value_or(0);
            out.pause = pauseTag != 0;
            out.hold = out.pause ? int(std::min<uint32_t>(pauseTag >> 1, 1u << 20)) : 0;
            out.width = kLineW;
            runScroll(p, out);
        } else {
            runCrawl(p, out);
        }
        return out;
    } catch (const std::exception& ex) {
        if (whyNot) *whyNot = ex.what();
        return std::nullopt;
    }
}

}  // namespace vt
