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
    const size_t end = std::min(f.offset + 8 + f.size, d.size());  // the form's end, inside the buffer
    walk(d, f.offset + 12, end, [&](std::array<char, 4> id, size_t o, size_t sz) {
        // walk() guarantees o + 8 <= end, so the clamp cannot underflow.
        out.push_back({id, o + 8, std::min(sz, end - (o + 8))});
    });
    return out;
}

}  // namespace vt
