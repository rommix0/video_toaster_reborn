// vt_render: renders one Auto transition of an effect to PPM frames (and the effect's sound to
// effect.wav) so the engine's output can be looked at without the app. No Qt, no FFmpeg.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "vt/crc32.h"
#include "vt/crouton.h"
#include "vt/effect.h"
#include "vt/framestore.h"
#include "vt/raster.h"
#include "vt/sound.h"
#include "vt/switcher.h"
#include "vt/timing.h"

namespace fs = std::filesystem;

namespace {

void usage(std::ostream& o) {
    o << "usage: vt_render <effect-path> --out <dir> [--speed slow|medium|fast|variable]\n"
         "                 [--a slide|bars] [--b slide|bars] [--underscan]\n"
         "\n"
         "Renders one Auto transition of the crouton at <effect-path> (Main = A, Preview = B)\n"
         "and writes NNNN.ppm frames (P6, from 0001) to <dir>, plus effect.wav (16-bit PCM at the\n"
         "effect's native rate) if the effect has a sound.\n"
         "\n"
         "  --speed     the speed button; omitted = the effect's own default speed\n"
         "  --a, --b    test picture for A (default slide) and B (default slide):\n"
         "                slide  solid colour with a lighter 64-pixel grid\n"
         "                       (A navy 0x1B2A80, B crimson 0xB5122E)\n"
         "                bars   75% colour bars\n"
         "  --underscan write the full 768x482 raster; default is the 640x432 visible area\n"
         "\n"
         "Frames are 59.94 fps, one per Switcher tick; 0001 shows the T-bar at 0. An ordinary\n"
         "effect renders until its transition ends, which is durationTicks[speed] frames (Page\n"
         "Peel ! at fast: 60). A ChromaFX or Overlays-drawer effect never ends by itself - it\n"
         "stays engaged - so exactly durationTicks[speed] frames are rendered: for an animated\n"
         "overlay that is one full loop of its animation over A.\n"
         "\n"
         "A FRAM crouton (a framestore) is not an effect: its picture is written to <dir>/still.png\n"
         "(the visible area, or the full raster with --underscan) and nothing else.\n";
}

vt::Frame makePicture(const std::string& kind, bool isB) {
    vt::Frame f;
    if (kind == "bars") {
        // 75% bars: white yellow cyan green magenta red blue black.
        static const uint8_t bars[8][3] = {{191, 191, 191}, {191, 191, 0}, {0, 191, 191}, {0, 191, 0},
                                           {191, 0, 191},   {191, 0, 0},   {0, 0, 191},   {0, 0, 0}};
        for (int y = 0; y < vt::kRasterH; ++y)
            for (int x = 0; x < vt::kRasterW; ++x) {
                const uint8_t* c = bars[x * 8 / vt::kRasterW];
                f.at(x, y) = {c[0], c[1], c[2], 255};
            }
        return f;
    }
    const uint32_t rgb = isB ? 0xB5122E : 0x1B2A80;
    const uint8_t r = uint8_t(rgb >> 16), g = uint8_t(rgb >> 8), b = uint8_t(rgb);
    auto lighter = [](uint8_t v) { return uint8_t(v + (255 - v) * 45 / 100); };
    const vt::Rgba8 base{r, g, b, 255}, line{lighter(r), lighter(g), lighter(b), 255};
    for (int y = 0; y < vt::kRasterH; ++y)
        for (int x = 0; x < vt::kRasterW; ++x) f.at(x, y) = (x % 64 < 2 || y % 64 < 2) ? line : base;
    return f;
}

bool writePpm(const fs::path& path, const vt::Frame& f, const vt::Rect& r) {
    std::ofstream o(path, std::ios::binary);
    if (!o) return false;
    o << "P6\n" << r.w << ' ' << r.h << "\n255\n";
    std::vector<char> row(size_t(r.w) * 3);
    for (int y = r.y; y < r.y + r.h; ++y) {
        for (int x = 0; x < r.w; ++x) {
            const vt::Rgba8& p = f.at(r.x + x, y);
            row[size_t(x) * 3] = char(p.r);
            row[size_t(x) * 3 + 1] = char(p.g);
            row[size_t(x) * 3 + 2] = char(p.b);
        }
        o.write(row.data(), std::streamsize(row.size()));
    }
    return bool(o);
}

void be32(std::vector<uint8_t>& v, uint32_t x) { for (int s = 24; s >= 0; s -= 8) v.push_back(uint8_t(x >> s)); }

void pngChunk(std::ostream& o, const char* type, const std::vector<uint8_t>& data) {
    std::vector<uint8_t> b;
    be32(b, uint32_t(data.size()));
    b.insert(b.end(), type, type + 4);
    b.insert(b.end(), data.begin(), data.end());
    const uint32_t crc = vt::crc32(std::span<const uint8_t>(b).subspan(4));
    be32(b, crc);
    o.write(reinterpret_cast<const char*>(b.data()), std::streamsize(b.size()));
}

// 8-bit RGB PNG with stored (uncompressed) deflate blocks: the engine has no zlib, and the sheet
// tools only need a file PIL opens.
bool writePng(const fs::path& path, const vt::Frame& f, const vt::Rect& r) {
    std::vector<uint8_t> raw;
    raw.reserve(size_t(r.w * 3 + 1) * size_t(r.h));
    for (int y = r.y; y < r.y + r.h; ++y) {
        raw.push_back(0);  // filter: none
        for (int x = 0; x < r.w; ++x) {
            const vt::Rgba8& p = f.at(r.x + x, y);
            raw.insert(raw.end(), {p.r, p.g, p.b});
        }
    }
    std::vector<uint8_t> z{0x78, 0x01};
    for (size_t at = 0; at < raw.size();) {
        const size_t n = std::min<size_t>(65535, raw.size() - at);
        z.push_back(at + n == raw.size() ? 1 : 0);
        z.insert(z.end(), {uint8_t(n), uint8_t(n >> 8), uint8_t(~n), uint8_t(~n >> 8)});
        z.insert(z.end(), raw.begin() + std::ptrdiff_t(at), raw.begin() + std::ptrdiff_t(at + n));
        at += n;
    }
    uint32_t a = 1, b = 0;
    for (uint8_t c : raw) { a = (a + c) % 65521; b = (b + a) % 65521; }
    be32(z, (b << 16) | a);

    std::ofstream o(path, std::ios::binary);
    if (!o) return false;
    const uint8_t sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    o.write(reinterpret_cast<const char*>(sig), 8);
    std::vector<uint8_t> ihdr;
    be32(ihdr, uint32_t(r.w));
    be32(ihdr, uint32_t(r.h));
    ihdr.insert(ihdr.end(), {8, 2, 0, 0, 0});  // 8-bit RGB, deflate, no filter scheme, no interlace
    pngChunk(o, "IHDR", ihdr);
    pngChunk(o, "IDAT", z);
    pngChunk(o, "IEND", {});
    return bool(o);
}

void le16(std::ostream& o, uint32_t v) { o.put(char(v)); o.put(char(v >> 8)); }
void le32(std::ostream& o, uint32_t v) { le16(o, v & 0xFFFF); le16(o, v >> 16); }

bool writeWav(const fs::path& path, const vt::Sound& s) {
    std::ofstream o(path, std::ios::binary);
    if (!o) return false;
    const uint32_t dataBytes = uint32_t(s.pcm.size() * 2), ch = uint32_t(s.channels), rate = uint32_t(s.rate);
    o.write("RIFF", 4); le32(o, 36 + dataBytes); o.write("WAVE", 4);
    o.write("fmt ", 4); le32(o, 16); le16(o, 1); le16(o, ch); le32(o, rate);
    le32(o, rate * ch * 2); le16(o, ch * 2); le16(o, 16);
    o.write("data", 4); le32(o, dataBytes);
    for (int16_t v : s.pcm) le16(o, uint16_t(v));
    return bool(o);
}

}  // namespace

int main(int argc, char** argv) {
    std::string effectPath, outDir, a = "slide", b = "slide";
    std::optional<vt::Speed> speed;
    bool underscan = false;
    auto needValue = [&](int& i, const char* opt) -> const char* {
        if (i + 1 >= argc) { std::cerr << "vt_render: " << opt << " needs a value\n"; return nullptr; }
        return argv[++i];
    };
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        const char* v = nullptr;
        if (arg == "--help" || arg == "-h") { usage(std::cout); return 0; }
        else if (arg == "--underscan") underscan = true;
        else if (arg == "--out") { if (!(v = needValue(i, "--out"))) return 2; outDir = v; }
        else if (arg == "--a") { if (!(v = needValue(i, "--a"))) return 2; a = v; }
        else if (arg == "--b") { if (!(v = needValue(i, "--b"))) return 2; b = v; }
        else if (arg == "--speed") {
            if (!(v = needValue(i, "--speed"))) return 2;
            std::string s = v;
            if (s == "slow") speed = vt::Speed::Slow;
            else if (s == "medium") speed = vt::Speed::Medium;
            else if (s == "fast") speed = vt::Speed::Fast;
            else if (s == "variable") speed = vt::Speed::Variable;
            else { std::cerr << "vt_render: unknown speed '" << s << "'\n"; return 2; }
        }
        else if (!arg.empty() && arg[0] == '-') { std::cerr << "vt_render: unknown option " << arg << "\n"; usage(std::cerr); return 2; }
        else if (effectPath.empty()) effectPath = arg;
        else { std::cerr << "vt_render: more than one effect path\n"; return 2; }
    }
    if (effectPath.empty() || outDir.empty()) { usage(std::cerr); return 2; }
    for (const std::string* k : {&a, &b})
        if (*k != "slide" && *k != "bars") { std::cerr << "vt_render: unknown picture '" << *k << "'\n"; return 2; }

    if (vt::isFramestore(fs::path(effectPath))) {
        std::string why;
        const std::optional<vt::Frame> still = vt::loadFramestore(fs::path(effectPath), &why);
        if (!still) { std::cerr << "vt_render: not a framestore: " << why << "\n"; return 1; }
        std::error_code mk;
        fs::create_directories(outDir, mk);
        if (mk) { std::cerr << "vt_render: cannot create " << outDir << ": " << mk.message() << "\n"; return 1; }
        const vt::Rect r = underscan ? vt::Rect{0, 0, vt::kRasterW, vt::kRasterH} : vt::kVisibleArea;
        if (!writePng(fs::path(outDir) / "still.png", *still, r)) { std::cerr << "vt_render: cannot write still.png\n"; return 1; }
        std::cout << "wrote still.png (" << r.w << "x" << r.h << ") of " << effectPath << " to " << outDir << "\n";
        return 0;
    }

    std::string whyNot;
    std::shared_ptr<const vt::Effect> effect = vt::loadEffect(fs::path(effectPath), &whyNot);
    if (!effect) {
        std::cerr << "vt_render: cannot load " << effectPath << ": " << (whyNot.empty() ? "unknown reason" : whyNot) << "\n";
        return 1;
    }

    std::error_code ec;
    fs::create_directories(outDir, ec);
    if (ec) { std::cerr << "vt_render: cannot create " << outDir << ": " << ec.message() << "\n"; return 1; }

    const vt::Timing timing = effect->timing();
    const vt::Speed sp = timing.resolve(speed.value_or(timing.defaultSpeed));
    const int duration = timing.durationTicks[size_t(sp)];
    // ChromaFX and overlays never end by themselves: render durationTicks frames (an animated
    // overlay's durationTicks is one full loop, frameCount x fieldsPerFrame).
    const bool fixedCount = effect->isChromaFx() || effect->isOverlay();

    const vt::Frame picA = makePicture(a, false), picB = makePicture(b, true);
    vt::SourceFrames src;
    src.inputs[0] = &picA;
    src.inputs[1] = &picB;

    vt::Switcher sw;
    sw.select(vt::Bus::Main, vt::Source::In1);
    sw.select(vt::Bus::Preview, vt::Source::In2);
    std::string whyNotSet;
    if (!sw.setEffect(effect, &whyNotSet)) {
        std::cerr << "vt_render: effect refused: " << whyNotSet << "\n";
        return 1;
    }
    sw.autoTransition(sp);
    if (const auto snd = sw.pendingSound(); snd && snd->rate > 0 && snd->channels > 0) {
        if (!writeWav(fs::path(outDir) / "effect.wav", *snd)) { std::cerr << "vt_render: cannot write effect.wav\n"; return 1; }
    }

    const vt::Rect area = underscan ? vt::Rect{0, 0, vt::kRasterW, vt::kRasterH} : vt::kVisibleArea;
    vt::Frame program;
    int n = 0;
    // Cap: an ordinary transition must end at `duration` ticks; never loop forever if it does not.
    const int cap = std::max(duration, 1) + 2;
    // The Switcher's contract (switcher.h): render the frame, then tick.
    do {
        sw.render(src, program);
        char name[32];
        std::snprintf(name, sizeof name, "%04d.ppm", ++n);
        if (!writePpm(fs::path(outDir) / name, program, area)) { std::cerr << "vt_render: cannot write " << name << "\n"; return 1; }
        sw.tick();
    } while (fixedCount ? n < duration : (sw.transitionActive() && n < cap));

    std::cout << "rendered " << n << " frames (" << area.w << "x" << area.h << ") of " << effect->name() << " to " << outDir << "\n";
    return 0;
}
