#pragma once
// Delta ops applied in place to a copy of the reference frame's bit planes. Each throws
// std::runtime_error exactly where the matching Python function raises ValueError, IndexError
// or struct.error; readAnim treats that as a damaged frame.
#include <cstdint>
#include <span>

#include "vt/ilbm.h"

namespace vt {

void applyDelta5(Planes& planes, std::span<const uint8_t> dl);
void applyDelta7(Planes& planes, std::span<const uint8_t> dl, uint32_t bits);
void applyDelta8(Planes& planes, std::span<const uint8_t> dl);

}  // namespace vt
