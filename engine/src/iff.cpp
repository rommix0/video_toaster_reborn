#include "vt/iff.h"

#include <algorithm>

namespace vt {
namespace {

uint32_t be32(std::span<const uint8_t> d, size_t o) {
    return (uint32_t(d[o]) << 24) | (uint32_t(d[o + 1]) << 16) | (uint32_t(d[o + 2]) << 8) | d[o + 3];
}

std::array<char, 4> tag4(std::span<const uint8_t> d, size_t o) {
    return {char(d[o]), char(d[o + 1]), char(d[o + 2]), char(d[o + 3])};
}

constexpr std::array<char, 4> kForm{'F', 'O', 'R', 'M'};
constexpr std::array<char, 4> kAnim{'A', 'N', 'I', 'M'};
constexpr std::array<char, 4> kIlbm{'I', 'L', 'B', 'M'};

// Visit each chunk (id, header offset, size) in [o, end); chunks are padded to even length.
template <class Fn>
void walk(std::span<const uint8_t> d, size_t o, size_t end, Fn fn) {
    end = std::min(end, d.size());
    while (o <= end && end - o >= 8) {
        size_t sz = be32(d, o + 4);
        fn(tag4(d, o), o, sz);
        size_t step = 8 + sz + (sz & 1);
        if (step > end - o) break;
        o += step;
    }
}

size_t findForm(std::span<const uint8_t> d, size_t from) {
    static const uint8_t pat[4] = {'F', 'O', 'R', 'M'};
    if (from >= d.size()) return SIZE_MAX;
    auto it = std::search(d.begin() + from, d.end(), pat, pat + 4);
    return it == d.end() ? SIZE_MAX : size_t(it - d.begin());
}

}  // namespace

std::vector<Form> iffForms(std::span<const uint8_t> d, size_t begin, size_t end) {
    std::vector<Form> out;
    walk(d, begin, end, [&](std::array<char, 4> id, size_t o, size_t sz) {
        if (id == kForm && o + 12 <= d.size()) out.push_back({tag4(d, o + 8), o, sz});
    });
    return out;
}

std::vector<Chunk> formChunks(std::span<const uint8_t> d, const Form& f) {
    std::vector<Chunk> out;
    walk(d, f.offset + 12, f.offset + 8 + f.size,
         [&](std::array<char, 4> id, size_t o, size_t sz) { out.push_back({id, o + 8, sz}); });
    return out;
}

std::vector<Form> unwrapHunk(std::span<const uint8_t> d) {
    std::vector<Form> found;
    if (d.size() < 4 || d[0] != 0 || d[1] != 0 || d[2] != 0x03 || d[3] != 0xf3) return found;
    size_t i = findForm(d, 0);
    while (i != SIZE_MAX && i + 12 <= d.size()) {
        size_t sz = be32(d, i + 4);
        auto ft = tag4(d, i + 8);
        if ((ft == kAnim || ft == kIlbm) && i + 8 + sz <= d.size() + 1) {
            found.push_back({ft, i, sz});
            i = findForm(d, i + 8 + sz);
        } else {
            i = findForm(d, i + 4);
        }
    }
    return found;
}

}  // namespace vt
