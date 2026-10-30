#include "vt/ilbm.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <stdexcept>

namespace vt {
namespace {

constexpr std::array<char, 4> kBmhd{'B', 'M', 'H', 'D'};
constexpr std::array<char, 4> kCmap{'C', 'M', 'A', 'P'};
constexpr std::array<char, 4> kCamg{'C', 'A', 'M', 'G'};
constexpr std::array<char, 4> kBody{'B', 'O', 'D', 'Y'};

// Refuse absurd BMHD-derived allocations (the largest real picture is well under 4 MB).
constexpr size_t kMaxBodyBytes = size_t(1) << 28;

const Chunk* find(const std::vector<Chunk>& chunks, std::array<char, 4> id) {
    for (auto& c : chunks)
        if (c.id == id) return &c;
    return nullptr;
}

// Payload of a chunk as a span, never past the end of the buffer.
std::span<const uint8_t> payload(std::span<const uint8_t> d, const Chunk& c) {
    if (c.data > d.size()) return {};
    return d.subspan(c.data, std::min(c.size, d.size() - c.data));
}

}  // namespace

std::vector<uint8_t> unpackByteRun1(std::span<const uint8_t> src, size_t outSize) {
    std::vector<uint8_t> out;
    out.reserve(outSize);
    size_t i = 0;
    while (out.size() < outSize) {
        if (i >= src.size()) throw std::runtime_error("ByteRun1: source ended before the output was full");
        const uint8_t c = src[i++];
        if (c < 128) {
            const size_t n = size_t(c) + 1;
            if (n > src.size() - i) throw std::runtime_error("ByteRun1: literal run past the end of the source");
            if (n > outSize - out.size()) throw std::runtime_error("ByteRun1: literal run past the output");
            out.insert(out.end(), src.begin() + i, src.begin() + i + n);
            i += n;
        } else if (c > 128) {
            const size_t n = 257 - size_t(c);
            if (i >= src.size()) throw std::runtime_error("ByteRun1: repeat run without a data byte");
            if (n > outSize - out.size()) throw std::runtime_error("ByteRun1: repeat run past the output");
            out.insert(out.end(), n, src[i++]);
        }
        // 128 is a no-op
    }
    return out;
}

std::vector<uint8_t> packByteRun1(std::span<const uint8_t> row) {
    std::vector<uint8_t> out;
    const size_t n = row.size();
    // Length (at most 128) of the run of equal bytes starting at i.
    auto run = [&](size_t i) {
        size_t r = 1;
        while (i + r < n && r < 128 && row[i + r] == row[i]) ++r;
        return r;
    };
    for (size_t i = 0; i < n;) {
        if (const size_t r = run(i); r >= 3) {
            out.push_back(uint8_t(257 - r));
            out.push_back(row[i]);
            i += r;
            continue;
        }
        const size_t start = i;
        while (i < n && i - start < 128 && run(i) < 3) ++i;
        out.push_back(uint8_t(i - start - 1));
        out.insert(out.end(), row.begin() + start, row.begin() + i);
    }
    return out;
}

Planes bodyToPlanes(const Bitmap& bm, std::span<const uint8_t> body) {
    if (bm.w < 0 || bm.h < 0 || bm.planes < 0) throw std::runtime_error("ILBM: negative dimensions");
    const size_t rb = size_t(bm.rowbytes());
    const size_t stored = size_t(bm.planes) + (bm.masking == 1 ? 1 : 0);  // planes per row in BODY
    const size_t size = rb * stored * size_t(bm.h);
    if (size > kMaxBodyBytes) throw std::runtime_error("ILBM: picture too large");

    std::vector<uint8_t> rawStore;
    std::span<const uint8_t> raw;
    if (bm.compression == 1) {
        rawStore = unpackByteRun1(body, size);
        raw = rawStore;
    } else if (bm.compression == 0) {
        if (body.size() < size) throw std::runtime_error("ILBM: BODY shorter than the bitmap");
        raw = body.first(size);
    } else {
        throw std::runtime_error("ILBM: unsupported compression");
    }

    Planes p;
    p.count = bm.planes;
    p.h = bm.h;
    p.rowbytes = int(rb);
    p.bytes.resize(size_t(bm.planes) * size_t(bm.h) * rb);
    for (size_t y = 0; y < size_t(bm.h); ++y)
        for (size_t k = 0; k < size_t(bm.planes); ++k)
            std::memcpy(p.plane(int(k)) + y * rb, raw.data() + (y * stored + k) * rb, rb);
    return p;
}

IndexImage planesToIndex(const Planes& planes, int w) {
    if (planes.count > 8) throw std::runtime_error("planesToIndex: more than 8 planes is not supported");
    if (w < 0 || size_t(w) > size_t(planes.rowbytes) * 8)
        throw std::runtime_error("planesToIndex: width exceeds the planes");
    IndexImage img;
    img.w = w;
    img.h = planes.h;
    img.px.assign(size_t(w) * size_t(planes.h), 0);
    for (int k = 0; k < planes.count; ++k) {
        const uint8_t* pl = planes.plane(k);
        for (int y = 0; y < planes.h; ++y) {
            const uint8_t* row = pl + size_t(y) * size_t(planes.rowbytes);
            uint8_t* dst = img.px.data() + size_t(y) * size_t(w);
            for (int x = 0; x < w; ++x) dst[x] |= uint8_t(((row[x >> 3] >> (7 - (x & 7))) & 1) << k);
        }
    }
    return img;
}

Bitmap readBitmapHeader(std::span<const uint8_t> d, const Form& form) {
    const auto chunks = formChunks(d, form);
    Bitmap bm;

    const Chunk* hd = find(chunks, kBmhd);
    if (!hd) throw std::runtime_error("ILBM: no BMHD");
    auto h = payload(d, *hd);
    if (h.size() < 20) throw std::runtime_error("ILBM: BMHD too short");
    bm.w = (h[0] << 8) | h[1];
    bm.h = (h[2] << 8) | h[3];
    bm.y = int16_t((h[6] << 8) | h[7]);
    bm.planes = h[8];
    bm.masking = h[9];
    bm.compression = h[10];

    if (const Chunk* cm = find(chunks, kCmap)) {
        auto c = payload(d, *cm);
        for (size_t i = 0; i + 3 <= c.size(); i += 3) bm.cmap.push_back({c[i], c[i + 1], c[i + 2]});
    }
    if (const Chunk* cg = find(chunks, kCamg)) {
        auto c = payload(d, *cg);
        if (c.size() < 4) throw std::runtime_error("ILBM: CAMG too short");
        bm.camg = (uint32_t(c[0]) << 24) | (uint32_t(c[1]) << 16) | (uint32_t(c[2]) << 8) | c[3];
    }
    return bm;
}

std::pair<Bitmap, Planes> readIlbmPlanes(std::span<const uint8_t> d, const Form& form) {
    Bitmap bm = readBitmapHeader(d, form);
    const auto chunks = formChunks(d, form);
    const Chunk* bo = find(chunks, kBody);
    if (!bo) throw std::runtime_error("ILBM: no BODY");
    Planes planes = bodyToPlanes(bm, payload(d, *bo));
    return {std::move(bm), std::move(planes)};
}

std::pair<Bitmap, IndexImage> readIlbm(std::span<const uint8_t> d, const Form& form) {
    auto [bm, planes] = readIlbmPlanes(d, form);
    IndexImage img = planesToIndex(planes, bm.w);
    return {std::move(bm), std::move(img)};
}

}  // namespace vt
