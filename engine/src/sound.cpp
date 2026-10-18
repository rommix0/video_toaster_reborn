#include "vt/sound.h"

#include <algorithm>
#include <array>
#include <stdexcept>

#include "vt/iff.h"

namespace vt {
namespace {

constexpr std::array<char, 4> kVhdr{'V', 'H', 'D', 'R'};
constexpr std::array<char, 4> kBody{'B', 'O', 'D', 'Y'};
constexpr std::array<char, 4> kChan{'C', 'H', 'A', 'N'};

uint32_t be32(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}

// Offset of the first "FORM" + size + "8SVX" in the file, or SIZE_MAX (as find_8svx in the Python).
size_t findSvx(std::span<const uint8_t> d) {
    static const uint8_t pat[4] = {'8', 'S', 'V', 'X'};
    auto it = d.begin();
    while (true) {
        it = std::search(it, d.end(), pat, pat + 4);
        if (it == d.end()) return SIZE_MAX;
        const size_t i = size_t(it - d.begin());
        if (i >= 8 && d[i - 8] == 'F' && d[i - 7] == 'O' && d[i - 6] == 'R' && d[i - 5] == 'M') return i - 8;
        ++it;
    }
}

}  // namespace

std::optional<Sound> readSound(std::span<const uint8_t> d) {
    const size_t fo = findSvx(d);
    if (fo == SIZE_MAX) return std::nullopt;
    const Form form{{'8', 'S', 'V', 'X'}, fo, be32(&d[fo + 4])};

    const Chunk* vh = nullptr;
    const Chunk* bo = nullptr;
    const Chunk* ch = nullptr;
    const auto chunks = formChunks(d, form);  // sizes are clamped to the buffer
    for (auto& c : chunks) {
        if (c.id == kVhdr && !vh) vh = &c;
        if (c.id == kBody && !bo) bo = &c;
        if (c.id == kChan && !ch) ch = &c;
    }
    if (!vh || vh->size < 20) throw std::runtime_error("8SVX: no VHDR");
    if (!bo) throw std::runtime_error("8SVX: no BODY");
    const uint8_t* v = d.data() + vh->data;
    const uint64_t one = be32(v), rep = be32(v + 4);
    const int rate = (v[12] << 8) | v[13];
    if (v[15]) throw std::runtime_error("8SVX: compressed samples are not supported");

    const uint8_t* body = d.data() + bo->data;
    const size_t bn = bo->size;
    const bool chan6 = ch && ch->size >= 4 && be32(d.data() + ch->data) == 6;

    const uint64_t n = one + rep;
    Sound s;
    s.rate = rate;
    if (chan6 || (n && bn >= 2 * n)) {
        const size_t half = bn / 2;
        s.channels = 2;
        s.pcm.resize(half * 2);
        for (size_t i = 0; i < half; ++i) {
            s.pcm[2 * i] = int16_t(int8_t(body[i]) * 256);
            s.pcm[2 * i + 1] = int16_t(int8_t(body[half + i]) * 256);
        }
    } else {
        const size_t cnt = n ? size_t(std::min<uint64_t>(n, bn)) : bn;
        s.channels = 1;
        s.pcm.resize(cnt);
        for (size_t i = 0; i < cnt; ++i) s.pcm[i] = int16_t(int8_t(body[i]) * 256);
    }
    return s;
}

}  // namespace vt
