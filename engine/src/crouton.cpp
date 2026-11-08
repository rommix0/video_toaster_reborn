#include "vt/crouton.h"

#include <algorithm>
#include <exception>
#include <fstream>
#include <iterator>
#include <string_view>

namespace vt {
namespace {

using Table = std::map<uint32_t, uint32_t>;

// Taglist defaults recorded from crouton.library. Keys are 0x80000000 | id.
Table make(std::initializer_list<std::pair<uint32_t, uint32_t>> l) {
    Table t;
    for (auto [k, v] : l) t[0x80000000u | k] = v;
    return t;
}

const Table& defaults(CroutonType t) {
    static const Table anim = make({{0x55, 1}, {0x23, 0}, {0x6b, 0x78}, {0x35, 1}, {0x6c, 0x12c},
                                    {0x88, 0x80000000}, {0x89, 0xffffffff}, {0x8a, 0xffffffff},
                                    {0x56, 0}, {0x5a, 0}, {0x57, 3}, {0x58, 2}, {0x59, 1},
                                    {0x5b, 3}, {0x5c, 2}, {0x5d, 1}, {0x5e, 0},
                                    {0x65, 0}, {0x66, 0}, {0x67, 0}, {0x68, 0}});
    static const Table ilbm = make({{0x55, 0}, {0x56, 0x3c}, {0x5a, 0x3c}, {0x23, 0}, {0x6b, 0x78},
                                    {0x35, 1}, {0x6c, 0x12c}, {0x57, 0x5a}, {0x58, 0x3c}, {0x59, 0x1e},
                                    {0x5b, 0x5a}, {0x5c, 0x3c}, {0x5d, 0x1e}, {0x75, 0x4650},
                                    {0x88, 0x80000000}, {0x89, 0xffffffff}, {0x8a, 0xffffffff},
                                    {0x5e, 0}, {0x65, 0}, {0x66, 0}, {0x67, 0}, {0x68, 0}});
    static const Table algo = [] {
        Table a = ilbm;
        a[0x80000025] = 0;
        return a;
    }();
    static const Table crfx = make({{0x6b, 0x78}, {0x35, 1}, {0x6c, 0x12c}, {0x75, 0xd2f0},
                                    {0x88, 0x80000000}, {0x89, 0xffffffff}, {0x8a, 0xffffffff}});
    static const Table none;
    switch (t) {
        case CroutonType::Anim: return anim;
        case CroutonType::Ilbm: return ilbm;
        case CroutonType::Algo: return algo;
        case CroutonType::Crfx: return crfx;
        default: return none;
    }
}

uint32_t be32(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}

CroutonType typeFrom(std::string_view s) {
    if (s == "ALGO") return CroutonType::Algo;
    if (s == "ILBM") return CroutonType::Ilbm;
    if (s == "ANIM") return CroutonType::Anim;
    if (s == "CRFX") return CroutonType::Crfx;
    if (s == "MAIN") return CroutonType::Main;
    if (s == "CTRL") return CroutonType::Ctrl;
    if (s == "FRAM") return CroutonType::Fram;
    if (s == "KEYF") return CroutonType::Keyf;
    if (s == "SCRO") return CroutonType::Scro;
    if (s == "CRAW") return CroutonType::Craw;
    return CroutonType::Other;
}

constexpr uint32_t kBlobBit = 0x10000000u;

// Port of toaster_iff.crud_info's TAGS walk.
void parseTags(Crouton& c, size_t o, size_t n) {
    const uint8_t* blob = c.bytes.data() + o;
    size_t i = 0;
    while (i + 8 <= n) {
        uint32_t k = be32(blob + i), v = be32(blob + i + 4);
        if (k == 0) break;
        if (k & kBlobBit) {
            size_t len = std::min<size_t>(v, n - (i + 8));
            c.blobs[k].assign(blob + i + 8, blob + i + 8 + len);
            i += 8 + size_t(v) + (v & 1);
        } else {
            c.tags[k] = v;
            i += 8;
        }
    }
}

}  // namespace

std::optional<uint32_t> Crouton::tag(uint32_t id) const {
    auto it = tags.find(id);
    if (it == tags.end()) return std::nullopt;
    return it->second;
}

uint32_t Crouton::tagOr(uint32_t id) const {
    if (auto v = tag(id)) return *v;
    const auto& d = defaults(type);
    auto it = d.find(id);
    return it == d.end() ? 0 : it->second;
}

std::optional<Crouton> loadCrouton(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return std::nullopt;
    Crouton c;
    c.bytes.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    std::span<const uint8_t> d(c.bytes);

    c.forms = iffForms(d);
    for (const Form& fm : c.forms) {
        if (std::string_view(fm.type.data(), 4) != "CrUD") continue;
        bool haveType = false, haveTags = false;
        for (const Chunk& ch : formChunks(d, fm)) {
            std::string_view id(ch.id.data(), 4);
            if (id == "TYPE" && !haveType && ch.data + 4 <= d.size()) {
                haveType = true;
                c.type = typeFrom(std::string_view(reinterpret_cast<const char*>(d.data() + ch.data), 4));
            } else if (id == "TAGS" && !haveTags) {
                haveTags = true;
                parseTags(c, ch.data, std::min(ch.size, d.size() - ch.data));
            }
        }
        break;  // first CrUD only, as crud_info
    }
    return c;
}

std::optional<CroutonType> croutonType(const std::filesystem::path& path) {
    try {
        const auto c = loadCrouton(path);
        if (!c) return std::nullopt;
        if (c->type != CroutonType::None) return c->type;
        for (const char* ext : {".i", ".I"}) {
            auto sib = path;
            sib += ext;
            if (const auto icon = loadCrouton(sib); icon && icon->type != CroutonType::None)  // a broken one has none
                return icon->type;
        }
        return CroutonType::None;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

}  // namespace vt
