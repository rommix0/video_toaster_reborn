#include "vt/assets/hunk.h"

#include <algorithm>
#include <stdexcept>
#include <string>

namespace vt::assets {
namespace {

constexpr uint32_t kCode = 0x3E9, kData = 0x3EA, kBss = 0x3EB, kReloc32 = 0x3EC,
                   kSymbol = 0x3F0, kDebug = 0x3F1, kEnd = 0x3F2, kHeader = 0x3F3, kBreak = 0x3F6,
                   kDRel32 = 0x3F7, kReloc32Short = 0x3FC, kRelRel32 = 0x3FD;
constexpr uint32_t kSizeMask = 0x3FFFFFFF;
constexpr uint64_t kBase = 0x1000, kAlign = 0x1000;  // same layout constants as hunk.py
constexpr uint64_t kMaxImage = 256u << 20;           // far above any real Toaster program

[[noreturn]] void bad(const std::string& why) { throw std::runtime_error("hunk file: " + why); }

class Reader {
public:
    explicit Reader(std::span<const uint8_t> d) : d_(d) {}
    uint64_t pos() const { return o_; }
    bool atEnd() const { return o_ >= d_.size(); }
    uint32_t u32() {
        need(4);
        uint32_t v = uint32_t(d_[o_]) << 24 | uint32_t(d_[o_ + 1]) << 16 | uint32_t(d_[o_ + 2]) << 8 | d_[o_ + 3];
        o_ += 4;
        return v;
    }
    uint16_t u16() {
        need(2);
        uint16_t v = uint16_t(d_[o_] << 8 | d_[o_ + 1]);
        o_ += 2;
        return v;
    }
    std::span<const uint8_t> take(uint64_t n) {
        need(n);
        auto s = d_.subspan(size_t(o_), size_t(n));
        o_ += n;
        return s;
    }
    void skipWords(uint64_t n) { take(n * 4); }
    void align4() { if (o_ & 2) take(2); }

private:
    std::span<const uint8_t> d_;
    uint64_t o_ = 0;
    void need(uint64_t n) const {
        if (n > d_.size() - std::min<uint64_t>(o_, d_.size())) bad("truncated at offset " + std::to_string(o_));
    }
};

}  // namespace

uint32_t HunkImage::be32(uint32_t addr) const {
    if (uint64_t(addr) + 4 > flat.size()) throw std::out_of_range("HunkImage::be32 past end of image");
    return uint32_t(flat[addr]) << 24 | uint32_t(flat[addr + 1]) << 16 | uint32_t(flat[addr + 2]) << 8 | flat[addr + 3];
}

uint16_t HunkImage::be16(uint32_t addr) const {
    if (uint64_t(addr) + 2 > flat.size()) throw std::out_of_range("HunkImage::be16 past end of image");
    return uint16_t(flat[addr] << 8 | flat[addr + 1]);
}

HunkImage loadHunks(std::span<const uint8_t> file) {
    Reader r(file);
    if (r.u32() != kHeader) bad("not a hunk executable");
    while (uint32_t n = r.u32()) r.skipWords(n);  // resident library names (none in practice)
    r.u32();                                      // table size; the first/last range is what counts
    const uint32_t first = r.u32(), last = r.u32();
    if (last < first) bad("bad hunk range");

    HunkImage img;
    uint64_t end = kBase;
    for (uint64_t i = first; i <= last; ++i) {
        const uint32_t s = r.u32();
        const uint32_t memFlags = s >> 30;
        if (memFlags == 3) r.u32();               // explicit memory attributes follow
        const uint64_t size = uint64_t(s & kSizeMask) * 4;
        const uint64_t base = end;
        end = (base + size + kAlign - 1) / kAlign * kAlign + kAlign;
        if (end > kMaxImage) bad("image too large");
        img.hunks.push_back({uint32_t(base), uint32_t(size), HunkImage::Hunk::Code, memFlags == 1});
    }
    img.flat.assign(size_t(end), 0);

    // Relocation entries add the target hunk's base to the 32-bit word at hunk `cur` + off.
    int cur = -1;
    std::vector<bool> seen(img.hunks.size(), false);
    auto current = [&]() -> const HunkImage::Hunk& {
        if (cur < 0) bad("relocation before any hunk");
        return img.hunks[size_t(cur)];
    };
    auto reloc = [&](uint32_t target, uint32_t off) {
        const auto& h = current();
        if (target >= img.hunks.size()) bad("relocation target out of range");
        if (uint64_t(off) + 4 > h.size) bad("relocation outside its hunk");
        uint8_t* p = &img.flat[h.base + off];
        uint32_t v = (uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]) + img.hunks[target].base;
        p[0] = uint8_t(v >> 24); p[1] = uint8_t(v >> 16); p[2] = uint8_t(v >> 8); p[3] = uint8_t(v);
    };
    auto nextHunk = [&](HunkImage::Hunk::Kind kind) -> HunkImage::Hunk& {
        if (size_t(cur + 1) >= img.hunks.size()) bad("more hunks than the header declares");
        auto& h = img.hunks[size_t(++cur)];
        h.kind = kind;
        seen[size_t(cur)] = true;
        return h;
    };

    bool done = false;
    while (!done && !r.atEnd()) {
        const uint32_t t = r.u32() & kSizeMask;
        switch (t) {
            case kCode:
            case kData: {
                const uint64_t n = uint64_t(r.u32() & kSizeMask) * 4;
                auto& h = nextHunk(t == kCode ? HunkImage::Hunk::Code : HunkImage::Hunk::Data);
                if (n > h.size) bad("hunk contents larger than the declared size");
                auto bytes = r.take(n);
                std::copy(bytes.begin(), bytes.end(), img.flat.begin() + h.base);
                break;
            }
            case kBss:
                r.u32();
                nextHunk(HunkImage::Hunk::Bss);
                break;
            case kReloc32:
            case kDRel32:
                while (uint32_t n = r.u32()) {
                    const uint32_t target = r.u32();
                    for (uint32_t k = 0; k < n; ++k) reloc(target, r.u32());
                }
                break;
            case kReloc32Short:
            case kRelRel32:
                while (uint32_t n = r.u16()) {
                    const uint32_t target = r.u16();
                    for (uint32_t k = 0; k < n; ++k) reloc(target, r.u16());
                }
                r.align4();
                break;
            case kSymbol:
                while (uint32_t n = r.u32()) {
                    r.skipWords(n);
                    r.u32();
                }
                break;
            case kDebug: r.skipWords(r.u32()); break;
            case kEnd: break;
            case kBreak: done = true; break;
            default: bad("unhandled hunk type " + std::to_string(t) + " at offset " + std::to_string(r.pos() - 4));
        }
    }
    if (!std::all_of(seen.begin(), seen.end(), [](bool b) { return b; })) bad("fewer hunks than the header declares");
    return img;
}

}  // namespace vt::assets
