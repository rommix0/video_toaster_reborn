#pragma once
#include <cstdint>
#include <vector>

#include "vt/raster.h"

namespace vt::assets {

struct IndexedImage {
    int w = 0, h = 0;
    std::vector<uint8_t> px;  // palette indices, row-major
};

// Amiga 4-bit-per-gun colours expanded to 8 bits (x17), alpha 255.
using Palette = std::vector<vt::Rgba8>;

}  // namespace vt::assets
