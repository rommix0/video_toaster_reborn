#include "vt/timing.h"

#include <algorithm>

namespace vt {
namespace {

// Slow, Medium, Fast, Variable.
constexpr uint32_t kTags[4] = {0x80000057u, 0x80000058u, 0x80000059u, 0x80000056u};
constexpr uint32_t kSpeedTag = 0x80000055u;

}  // namespace

Speed decodeSpeedTag(uint32_t v) {
    switch (v & 3u) {
        case 0: return Speed::Medium;
        case 1: return Speed::Fast;
        case 2: return Speed::Variable;
        default: return Speed::Slow;
    }
}

Speed Timing::resolve(Speed requested) const {
    if (available(requested)) return requested;
    if (available(defaultSpeed)) return defaultSpeed;
    for (Speed s : {Speed::Fast, Speed::Medium, Speed::Slow, Speed::Variable})
        if (available(s)) return s;
    return defaultSpeed;
}

Timing animTiming(const Crouton& c, int frameCount) {
    static constexpr uint32_t kDefault[4] = {3, 2, 1, 0};
    Timing t;
    for (size_t i = 0; i < 4; ++i) {
        int fields = static_cast<int>(c.tag(kTags[i]).value_or(kDefault[i]));
        t.fieldsPerFrame[i] = fields;
        t.durationFrames[i] = (fields * frameCount + 1) / 2;
    }
    t.defaultSpeed = decodeSpeedTag(c.tag(kSpeedTag).value_or(1));
    return t;
}

Timing fixedTiming(const Crouton& c) {
    static constexpr uint32_t kDefault[4] = {0x5a, 0x3c, 0x1e, 0x3c};
    Timing t;
    for (size_t i = 0; i < 4; ++i) t.durationFrames[i] = static_cast<int>(c.tag(kTags[i]).value_or(kDefault[i]));
    t.defaultSpeed = decodeSpeedTag(c.tag(kSpeedTag).value_or(0));
    return t;
}

int animFrameForTBar(int tbar, int frameCount) {
    if (frameCount <= 0) return 0;
    return std::min(frameCount - 1, static_cast<int>(static_cast<int64_t>(tbar) * frameCount / 512));
}

}  // namespace vt
