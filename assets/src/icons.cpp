#include "vt/assets/icons.h"

#include <algorithm>
#include <cstring>
#include <exception>
#include <fstream>
#include <iterator>
#include <optional>

#include "vt/ilbm.h"
#include "vt/log.h"

namespace vt::assets {
namespace {

namespace fs = std::filesystem;

std::string utf8(const fs::path& p) {
    const auto u8 = p.u8string();
    return {u8.begin(), u8.end()};
}

std::string lower(std::string s) {
    for (auto& c : s) c = char(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
    return s;
}

bool isIcon(const std::string& name) { return name.size() > 2 && lower(name.substr(name.size() - 2)) == ".i"; }

std::vector<uint8_t> readFile(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

bool startsWith(const std::vector<uint8_t>& d, size_t at, const char* tag) {
    return d.size() >= at + 4 && std::memcmp(d.data() + at, tag, 4) == 0;
}

// See scanCroutons: the stand-in for a ChromaFX crouton's picture.
void placeholder(Crouton& c) {
    c.image = {kCroutonW, kCroutonH, std::vector<uint8_t>(size_t(kCroutonW) * kCroutonH, 2)};
    for (int y = 0; y < kCroutonH; ++y)
        for (int x = 0; x < kCroutonW; ++x)
            if (x == 0 || y == 0 || x == kCroutonW - 1 || y == kCroutonH - 1) c.image.px[size_t(y) * kCroutonW + size_t(x)] = 0;
    c.palette = {{0x00, 0x00, 0x00, 255}, {0x55, 0x55, 0x55, 255}, {0x99, 0x99, 0x99, 255}, {0xdd, 0xdd, 0xdd, 255}};
}

void refuse(const fs::path& effect, const std::string& why) { logWarning("crouton " + utf8(effect) + ": " + why); }

// The crouton for one effect file, or nothing (logged for IFF files, see scanCroutons).
std::optional<Crouton> crouton(const fs::path& effect, const fs::path& icon) {
    Crouton c{effect, icon, utf8(effect.filename()), {}, {}};
    const auto d = readFile(c.icon);
    if (!startsWith(d, 0, "FORM") || !startsWith(d, 8, "ILBM")) {
        if (c.icon != c.effect) {  // a sibling icon without a picture (ChromaFx)
            placeholder(c);
            return c;
        }
        if (startsWith(d, 0, "FORM")) refuse(effect, "no icon (no sibling .i and no leading ILBM)");
        return std::nullopt;
    }
    try {
        const auto forms = iffForms(d);
        const auto [bm, img] = readIlbm(d, forms.front());
        if (bm.w != kCroutonW || bm.h != kCroutonH) {
            refuse(effect, "icon is " + std::to_string(bm.w) + "x" + std::to_string(bm.h) + ", not 80x50");
            return std::nullopt;
        }
        if (bm.cmap.empty()) {
            refuse(effect, "icon has no CMAP");
            return std::nullopt;
        }
        c.image = {img.w, img.h, img.px};
        for (const auto& rgb : bm.cmap) c.palette.push_back({rgb.r, rgb.g, rgb.b, 255});
        return c;
    } catch (const std::exception& e) {
        refuse(effect, std::string("icon not decoded (") + e.what() + ")");
        return std::nullopt;
    }
}

}  // namespace

std::vector<Crouton> scanCroutons(const fs::path& folder) {
    // Names as stored: the file system may ignore case, so siblings are matched in this list.
    std::vector<std::string> files, icons;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(folder, ec))
        if (std::error_code fe; e.is_regular_file(fe)) {
            std::string name = utf8(e.path().filename());
            (isIcon(name) ? icons : files).push_back(std::move(name));
        }
    std::sort(files.begin(), files.end(), [](const std::string& a, const std::string& b) {
        const std::string la = lower(a), lb = lower(b);
        return la != lb ? la < lb : a < b;
    });
    auto path = [&](const std::string& name) { return folder / fs::path(std::u8string(name.begin(), name.end())); };
    std::vector<Crouton> out;
    for (const auto& f : files) {
        fs::path icon = path(f);
        for (const char* ext : {".i", ".I"})
            if (std::find(icons.begin(), icons.end(), f + ext) != icons.end()) {
                icon = path(f + ext);
                break;
            }
        if (auto c = crouton(path(f), icon)) out.push_back(std::move(*c));
    }
    return out;
}

}  // namespace vt::assets
