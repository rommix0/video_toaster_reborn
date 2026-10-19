#include "vt/assets/panel.h"

#include <algorithm>
#include <cstdio>
#include <set>
#include <stdexcept>

namespace vt::assets {
namespace {

// All addresses are hunk.py flat-image addresses of 4.2/Programs/switcher.main (SCREEN.md).
constexpr uint32_t kA5 = 0x5d5c;                                   // small-data base
constexpr uint32_t kListHeads[9] = {0x158, 0x15c, 0x160, 0x164, 0x16c, 0x170, 0x174, 0x178, 0x17c};
constexpr uint32_t kExtraNodes[2] = {0x4a15c, 0x4e328};           // speed pop-up, pop-up marker: not on a list
constexpr uint32_t kPaletteRgb = 0x2bf3a, kPalettePreview = 0x2bf30;
constexpr uint32_t kBkgStrings = 0x11448a;                         // BKG pop-up string table (0x114432)
constexpr uint32_t kKeyLevelIText = 0x641b0;                       // key level IntuiText (0x064188)
constexpr int kWordsPerColumn[6] = {4, 2, 2, 1, 4, 3};             // per draw method (+0x2c / 4)
constexpr int kTbarStates = 86;
constexpr size_t kMaxNodes = 1000, kMaxStates = 200, kMaxString = 64;

// Node ids, identified by eye from the decoded images (render_layout.py NAMES).
struct Name { uint32_t node; const char* id; };
constexpr Name kNames[] = {
    {0x28000, "setup"}, {0x28074, "mode_dyn_160_244"}, {0x280ec, "gfx_paint"}, {0x28164, "gfx_cg"},
    {0x281dc, "gfx_3d"}, {0x28254, "mode_dyn_704_291"},
    {0x35000, "sup_1"}, {0x35074, "sup_2"}, {0x350e8, "sup_3"}, {0x3515c, "sup_4"}, {0x351d0, "sup_dv1"},
    {0x35248, "sup_dv2"}, {0x352c0, "sup_bkg"}, {0x35338, "hdr_superimpose"},
    {0x37000, "main_1"}, {0x37074, "main_2"}, {0x370e8, "main_3"}, {0x3715c, "main_4"}, {0x371d0, "main_dv1"},
    {0x37244, "main_dv2"}, {0x372b8, "main_bkg"}, {0x3732c, "lbl_main"},
    {0x3a000, "prev_1"}, {0x3a074, "prev_2"}, {0x3a0e8, "prev_3"}, {0x3a15c, "prev_4"}, {0x3a1d0, "prev_dv1"},
    {0x3a244, "prev_dv2"}, {0x3a2b8, "prev_bkg"}, {0x3a32c, "lbl_preview"}, {0x3a3a0, "logo"},
    {0x3a414, "hdr_video_controls"},
    {0x48000, "auto"}, {0x48076, "take"}, {0x480ec, "noimg_656_244"}, {0x4815e, "bkg_popup"},
    {0x43000, "hdr_framestore"}, {0x43074, "framestore_frame"}, {0x430e8, "fs_freeze"}, {0x43162, "fs_live"},
    {0x431dc, "fs_1"}, {0x43256, "fs_2"}, {0x432d0, "fs_3"}, {0x4334a, "fs_4"}, {0x433c4, "fs_stabilize"},
    {0x43438, "fs_save"},
    {0x4e000, "tbar"}, {0x4e1c8, "tbar_dir"}, {0x4e240, "tbar_cap_top"}, {0x4e2b4, "tbar_cap_bottom"},
    {0x4a000, "speed_s"}, {0x4a074, "speed_m"}, {0x4a0e8, "speed_f"}, {0x4a15c, "speed_popup"},
    {0x3f000, "gfx_3f000"}, {0x3f074, "gfx_dyn_544"}, {0x3f0f4, "gfx_dyn_560"}, {0x3f170, "gfx_dyn_624"},
    {0x3f1ec, "gfx_dyn_640"}, {0x3f268, "gfx_file_popup"}, {0x3f31a, "hdr_graphics"}, {0x3f38e, "gfx_3f38e"},
    {0x3f402, "gfx_load"},
    {0x4c000, "key_level_popup"}, {0x4c074, "key_level_noimg"}, {0x4c0e4, "key_dyn_416_270"},
    {0x4c158, "key_off"}, {0x4c1d0, "key_black"}, {0x4c248, "key_white"},
    {0x4e328, "popup_marker"},
};

std::string hex(uint32_t a) {
    char b[16];
    std::snprintf(b, sizeof b, "0x%06x", unsigned(a));
    return b;
}

[[noreturn]] void bad(uint32_t addr, const std::string& why) {
    throw std::runtime_error("switcher.main panel at " + hex(addr) + ": " + why);
}

// Bounds-checked reads that name the address on failure.
class Reader {
public:
    explicit Reader(const HunkImage& h) : h_(h) {}
    uint8_t u8(uint32_t a) const {
        if (a >= h_.flat.size()) bad(a, "read past the end of the image");
        return h_.flat[a];
    }
    uint16_t u16(uint32_t a) const {
        try { return h_.be16(a); } catch (const std::out_of_range&) { bad(a, "read past the end of the image"); }
    }
    int16_t s16(uint32_t a) const { return int16_t(u16(a)); }
    uint32_t u32(uint32_t a) const {
        try { return h_.be32(a); } catch (const std::out_of_range&) { bad(a, "read past the end of the image"); }
    }
    std::string cstr(uint32_t a) const {
        std::string s;
        for (uint32_t p = a; char c = char(u8(p)); ++p) {
            if (s.size() == kMaxString) bad(a, "unterminated string");
            s += c;
        }
        return s;
    }

private:
    const HunkImage& h_;
};

// One image as the 68000 draw methods at 0x17574 lay it out: rows x words columns of
// kWordsPerColumn[method] words. Colour = p0 | p1 << 1; methods 4/5 lead with the mask word.
void decode(const Reader& rd, int method, int words, int rows, uint32_t addr, IndexedImage* img, IndexedImage* mask) {
    const int wpc = kWordsPerColumn[method];
    const bool masked = method >= 4;
    img->w = words * 16;
    img->h = rows;
    img->px.assign(size_t(img->w) * size_t(rows), 0);
    if (mask) *mask = {img->w, rows, std::vector<uint8_t>(img->px.size())};
    size_t o = 0;
    uint32_t p = addr;
    for (int r = 0; r < rows; ++r)
        for (int w = 0; w < words; ++w, p += 2 * uint32_t(wpc)) {
            const uint16_t w0 = rd.u16(p);
            const uint16_t m = masked ? w0 : 0;
            const uint16_t p0 = masked ? rd.u16(p + 2) : w0;
            const uint16_t p1 = masked ? rd.u16(p + 4) : (method == 0 || method == 2) ? rd.u16(p + 2) : w0;
            for (int b = 15; b >= 0; --b, ++o) {
                img->px[o] = uint8_t((p0 >> b & 1) | (p1 >> b & 1) << 1);
                if (mask) mask->px[o] = uint8_t(m >> b & 1);
            }
        }
}

const char* nameOf(uint32_t node) {
    for (const auto& n : kNames)
        if (n.node == node) return n.id;
    return nullptr;
}

// One panel node (SCREEN.md section 2).
PanelNode readNode(const Reader& rd, uint32_t n, int list) {
    PanelNode out;
    const char* id = nameOf(n);
    out.id = id ? id : "node_" + hex(n).substr(2);
    out.list = list;
    const uint16_t m4 = rd.u16(n + 0x2c);
    if (m4 % 4 || m4 / 4 > 5) bad(n + 0x2c, "draw method " + std::to_string(m4) + " is not 0..5 x4");
    out.method = m4 / 4;
    const uint16_t words1 = rd.u16(n + 0x2e);
    const int words = words1 == 0xffff ? 0 : words1 + 1;           // 0xffff: width set at run time
    const int rows = rd.u16(n + 0x30) + 1;
    if (words * 16 > 768 || rows > 481) bad(n + 0x2e, "image larger than the screen");
    const uint32_t off = rd.u32(n + 0x34);                          // y*96 + x/8 in the plane
    out.rect = {int(off % 96 * 8), int(off / 96), words * 16, rows};
    out.hitRect = {rd.s16(n + 4), rd.s16(n + 6), rd.s16(n + 8), rd.s16(n + 10)};
    out.hiddenByDefault = rd.u8(n + 0x49) != 0;
    out.selectPattern = uint16_t(rd.u8(n + 0x4b) * 0x101);

    const bool masked = out.method >= 4;
    if (const uint32_t cur = rd.u32(n + 0x38))
        decode(rd, out.method, words, rows, cur, &out.image, nullptr);
    const uint16_t nst = rd.u16(n + 0x3c);
    if (const uint32_t tab = rd.u32(n + 0x40)) {
        if (nst >= kMaxStates) bad(n + 0x3c, "implausible state count " + std::to_string(nst));
        out.states.resize(nst);
        if (masked) out.masks.resize(nst);
        for (uint16_t s = 0; s < nst; ++s)
            decode(rd, out.method, words, rows, rd.u32(tab + 4u * s), &out.states[s], masked ? &out.masks[s] : nullptr);
    }
    return out;
}

Palette readPalette(const Reader& rd, uint32_t addr) {
    Palette p;
    for (uint32_t i = 0; i < 4; ++i) {
        const uint16_t w = rd.u16(addr + 2 * i);
        if (w > 0xfff) bad(addr + 2 * i, "not a 12-bit colour");
        p.push_back({uint8_t((w >> 8 & 15) * 17), uint8_t((w >> 4 & 15) * 17), uint8_t((w & 15) * 17), 255});
    }
    return p;
}

const PanelNode& findNode(const PanelLayout& l, const char* id, uint32_t addr) {
    for (const auto& n : l.nodes)
        if (n.id == id) return n;
    bad(addr, std::string("node ") + id + " not found");
}

}  // namespace

PanelLayout loadPanel(const HunkImage& switcherMain) {
    const Reader rd(switcherMain);
    PanelLayout l;
    std::set<uint32_t> seen;
    for (int i = 0; i < 9; ++i)
        for (uint32_t n = rd.u32(kA5 + kListHeads[i]); n && seen.insert(n).second; n = rd.u32(n)) {
            if (seen.size() > kMaxNodes) bad(n, "too many panel nodes");
            l.nodes.push_back(readNode(rd, n, i));
        }
    for (uint32_t n : kExtraNodes)
        if (seen.insert(n).second) {
            l.nodes.push_back(readNode(rd, n, -1));
            l.nodes.back().hiddenByDefault = true;  // drawn only on demand (code 0x1180c / 0x15000)
        }

    l.rgbMode = readPalette(rd, kPaletteRgb);          // used when $1d4(a5) == 0 (Control Panel RGB)
    l.previewMode = readPalette(rd, kPalettePreview);  // $1d4(a5) != 0 (PREVIEW; Toaster-encoded)

    // T-bar: state = $1ec(a5) / 6 for positions 0..511.
    const auto& tbar = findNode(l, "tbar", 0x4e000);
    if (tbar.states.size() != kTbarStates) bad(0x4e03c, "T-bar does not have 86 states");
    l.tbarImages = tbar.states;
    l.tbarRect = tbar.rect;

    // Run-time texts (SCREEN.md section 3).
    const auto& bkg = findNode(l, "bkg_popup", 0x4815e);
    l.texts.push_back({"bkg_popup_text", bkg.rect.x + 6, bkg.rect.y + 15, true,  // Move(x+6, y+15), 0x114432
                       rd.cstr(rd.u32(kBkgStrings)), false});
    const auto& speed = findNode(l, "speed_popup", 0x4a15c);
    l.texts.push_back({"speed_popup_text", speed.rect.x + 24, speed.rect.y + 15, true, "", true});  // 0x0622da
    // PrintIText(rp, IntuiText, 0, 0): LeftEdge/TopEdge are screen positions; the level (0..257,
    // default 128) is formatted into the IntuiText's buffer at run time.
    l.texts.push_back({"key_level_text", rd.s16(kKeyLevelIText + 4), rd.s16(kKeyLevelIText + 6), false, "128", false});
    return l;
}

IndexedImage selectedLook(const PanelNode& node, int state) {
    if (state < 0 || size_t(state) >= node.states.size()) throw std::out_of_range("selectedLook: no such state");
    IndexedImage img = node.states[size_t(state)];
    if (node.method >= 4) {
        static constexpr uint8_t kRotate[4] = {0, 2, 3, 1};  // plane0' = p1, plane1' = p0 ^ p1
        const auto& mask = node.masks[size_t(state)].px;
        for (size_t i = 0; i < img.px.size(); ++i)
            if (mask[i]) img.px[i] = kRotate[img.px[i]];
        return img;
    }
    // XOR both planes with the pattern word, rotated right by 2 on odd rows (as render_layout.py
    // approximates the bitmap-row parity rule; exact for pattern 0xffff).
    const uint16_t pat = node.selectPattern;
    const uint16_t alt = uint16_t(pat >> 2 | pat << 14);
    for (int y = 0; y < img.h; ++y) {
        const uint16_t d = y % 2 ? alt : pat;
        for (int x = 0; x < img.w; ++x)
            if (d >> (15 - x % 16) & 1) img.px[size_t(y) * size_t(img.w) + size_t(x)] ^= 3;
    }
    return img;
}

IndexedImage renderDefaultScreen(const PanelLayout& l) {
    IndexedImage scr{l.screenW, l.screenH, std::vector<uint8_t>(size_t(l.screenW) * size_t(l.screenH), 0)};
    for (int y = 0; y < l.screenH; ++y)  // blitter clear 0x240b6: colour 1 between 8-px colour-0 margins
        std::fill_n(scr.px.begin() + y * l.screenW + l.clearMarginX, l.screenW - 2 * l.clearMarginX,
                    uint8_t(l.clearColour));
    for (const auto& n : l.nodes) {
        if (n.hiddenByDefault || n.image.px.empty()) continue;
        for (int r = 0; r < n.image.h; ++r) {
            const int y = n.rect.y + r;
            if (y < 0 || y >= l.screenH) continue;
            for (int c = 0; c < n.image.w; ++c) {
                const int x = n.rect.x + c;
                if (x >= 0 && x < l.screenW)
                    scr.px[size_t(y) * size_t(l.screenW) + size_t(x)] = n.image.px[size_t(r) * size_t(n.image.w) + size_t(c)];
            }
        }
    }
    return scr;
}

}  // namespace vt::assets
