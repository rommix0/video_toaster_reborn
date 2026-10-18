#include "vt/anim.h"

#include <array>
#include <set>
#include <stdexcept>
#include <string>

#include "delta.h"

namespace vt {
namespace {

constexpr std::array<char, 4> kIlbm{'I', 'L', 'B', 'M'};
constexpr std::array<char, 4> kAnhd{'A', 'N', 'H', 'D'};
constexpr std::array<char, 4> kBody{'B', 'O', 'D', 'Y'};
constexpr std::array<char, 4> kDlta{'D', 'L', 'T', 'A'};
constexpr std::array<char, 4> kCmap{'C', 'M', 'A', 'P'};

const Chunk* find(const std::vector<Chunk>& chunks, std::array<char, 4> id) {
    for (auto& c : chunks)
        if (c.id == id) return &c;
    return nullptr;
}

std::span<const uint8_t> payload(std::span<const uint8_t> d, const Chunk& c) {
    if (c.data > d.size()) return {};
    return d.subspan(c.data, std::min(c.size, d.size() - c.data));
}

uint32_t be32(std::span<const uint8_t> d, size_t o) {
    return (uint32_t(d[o]) << 24) | (uint32_t(d[o + 1]) << 16) | (uint32_t(d[o + 2]) << 8) | d[o + 3];
}

}  // namespace

Anim readAnim(std::span<const uint8_t> d, const Form& form) {
    std::vector<Form> subs;
    for (const Form& f : iffForms(d, form.offset + 12, form.offset + 8 + form.size))
        if (f.type == kIlbm) subs.push_back(f);
    if (subs.empty()) throw std::runtime_error("ANIM: no ILBM sub-forms");

    Anim a;
    std::set<int> unsupported;
    bool haveIndices = true;
    for (size_t k = 0; k < subs.size(); ++k) {
        const auto sc = formChunks(d, subs[k]);
        if (k == 0) {
            a.bm = readBitmapHeader(d, subs[0]);
            const Chunk* bo = find(sc, kBody);
            if (!bo) throw std::runtime_error("ANIM: first frame has no BODY");
            a.planes.push_back(bodyToPlanes(a.bm, payload(d, *bo)));
            haveIndices = a.bm.planes <= 8;  // IndexImage holds a byte per pixel
            if (haveIndices) a.frames.push_back(planesToIndex(a.planes[0], a.bm.w));
            a.reltimes.push_back(0);
            if (a.bm.cmap.empty()) a.cmaps.emplace_back();
            else a.cmaps.emplace_back(a.bm.cmap);
            continue;
        }
        const Chunk* ah = find(sc, kAnhd);
        if (!ah) throw std::runtime_error("ANIM: sub-frame has no ANHD");
        if (ah->data + 24 > d.size()) throw std::runtime_error("ANIM: ANHD past the end of the file");
        const int op = d[ah->data];
        const int reltime = int(be32(d, ah->data + 14));
        const size_t interleave = d[ah->data + 18] ? d[ah->data + 18] : 2;
        const uint32_t bits = be32(d, ah->data + 20);

        // Frame n is a delta from frame n - interleave; until that many frames exist, from the one before.
        const size_t n = a.planes.size();
        Planes target = a.planes[n >= interleave ? n - interleave : n - 1];
        const Chunk* body = find(sc, kBody);
        const Chunk* dlta = find(sc, kDlta);
        if (op == 0 && body) {
            target = bodyToPlanes(a.bm, payload(d, *body));  // outside Python's try: failures stay fatal
        } else if (dlta) {
            const auto dl = payload(d, *dlta);
            try {
                if (op == 5) applyDelta5(target, dl);
                else if (op == 7) applyDelta7(target, dl, bits);
                else if (op == 8) applyDelta8(target, dl);
                else unsupported.insert(op);
            } catch (const std::runtime_error&) {
                if (k + 2 < subs.size()) throw;  // only a damaged last frame or two is forgiven
                a.dropped = int(subs.size() - k);
                break;
            }
        }
        a.planes.push_back(std::move(target));
        std::optional<std::vector<Rgb>> cm;
        if (const Chunk* cmap = find(sc, kCmap)) {
            auto c = payload(d, *cmap);
            cm.emplace();
            for (size_t i = 0; i + 3 <= c.size(); i += 3) cm->push_back({c[i], c[i + 1], c[i + 2]});
        }
        if (haveIndices) a.frames.push_back(planesToIndex(a.planes.back(), a.bm.w));
        a.reltimes.push_back(reltime);
        a.cmaps.push_back(std::move(cm));
    }
    if (!unsupported.empty()) {
        std::string s;
        for (int op : unsupported) s += (s.empty() ? "" : ", ") + std::to_string(op);
        throw std::runtime_error("ANIM: unsupported ops " + s);
    }
    return a;
}

}  // namespace vt
