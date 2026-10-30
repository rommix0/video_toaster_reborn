#pragma once
// Toaster "crouton" files: IFF FORM CrUD tags plus the FORMs they carry.
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <vector>

#include "vt/iff.h"

namespace vt {

enum class CroutonType { Algo, Ilbm, Anim, Crfx, Main, Ctrl, Fram, Keyf, Other, None };

struct Crouton {
    CroutonType type = CroutonType::None;
    std::vector<uint8_t> bytes;
    std::vector<Form> forms;  // all FORMs: top level, or those from unwrapHunk for hunk files
    std::map<uint32_t, uint32_t> tags;
    std::map<uint32_t, std::vector<uint8_t>> blobs;  // tags with bit 0x10000000 (byte blobs)

    std::optional<uint32_t> tag(uint32_t id) const;
    // Value of the tag, else the per-type crouton.library default, else 0.
    uint32_t tagOr(uint32_t id) const;
};

std::optional<Crouton> loadCrouton(const std::filesystem::path& p);

}  // namespace vt
