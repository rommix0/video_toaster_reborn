#pragma once
// Toaster "crouton" files: IFF FORM CrUD tags plus the FORMs they carry.
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <vector>

#include "vt/iff.h"

namespace vt {

enum class CroutonType { Algo, Ilbm, Anim, Crfx, Main, Ctrl, Fram, Keyf, Scro, Craw, Other, None };

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

// The crouton TYPE of `path` from its own CrUD, or with none its sibling icon's (<name>.i / .I; Ruling 9:
// the original reads a crouton's CrUD from its icon); None when neither has one, nullopt when the file
// does not read or does not parse.
std::optional<CroutonType> croutonType(const std::filesystem::path& path);

}  // namespace vt
