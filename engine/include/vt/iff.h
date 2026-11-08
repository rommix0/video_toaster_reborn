#pragma once
// IFF walking (port of tools/toaster_iff.py).
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace vt {

struct Form {
    std::array<char, 4> type;  // form type, e.g. ANIM, ILBM, CrUD
    size_t offset;             // start of the FORM header
    size_t size;               // big-endian size field at offset+4
};

struct Chunk {
    std::array<char, 4> id;
    size_t data;  // offset of the chunk payload
    size_t size;  // payload size (unpadded)
};

// Top-level FORMs between begin and end (chunks padded to even length).
std::vector<Form> iffForms(std::span<const uint8_t> d, size_t begin = 0, size_t end = SIZE_MAX);

// Chunks directly inside one FORM (after the 4-byte form type).
std::vector<Chunk> formChunks(std::span<const uint8_t> d, const Form& f);

}  // namespace vt
