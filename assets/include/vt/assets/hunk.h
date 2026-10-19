#pragma once
#include <cstdint>
#include <span>
#include <vector>

namespace vt::assets {

// An Amiga hunk executable (or a font/overlay file in hunk format) laid out in one flat image the
// way tools/ref/hunk.py does it: hunk i sits at an aligned base, RELOC32 entries are applied so
// absolute pointers in the image are flat addresses.
struct HunkImage {
    std::vector<uint8_t> flat;            // all hunks laid out as tools/ref/hunk.py does, relocs applied
    struct Hunk {
        uint32_t base, size;              // flat address and size in bytes
        enum Kind { Code, Data, Bss } kind;
        bool chip;                        // the header asked for chip memory
    };
    std::vector<Hunk> hunks;
    uint32_t be32(uint32_t addr) const;   // bounds-checked big-endian reads (throw std::out_of_range)
    uint16_t be16(uint32_t addr) const;
};

// Throws std::runtime_error with a reason for anything that is not a well-formed hunk file.
HunkImage loadHunks(std::span<const uint8_t> file);

}  // namespace vt::assets
