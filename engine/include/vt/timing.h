#pragma once
// Effect timing: how long an effect runs at each speed button, and the T-bar mapping.
#include <array>

#include "vt/crouton.h"

namespace vt {

// Index order of Timing's arrays. Tag 0x80000055 is NOT this order: the Switcher decodes
// it through a table at 0xe660c (see decodeSpeedTag).
enum class Speed { Slow = 0, Medium = 1, Fast = 2, Variable = 3 };

struct Timing {
    // ANIM content: video fields per animation frame (tags 0x57/0x58/0x59/0x56). Zero for
    // gradient content.
    std::array<int, 4> fieldsPerFrame{};
    // Effect length in frames at each speed. 0 means the speed is unavailable.
    std::array<int, 4> durationFrames{};
    Speed defaultSpeed = Speed::Medium;

    bool available(Speed s) const { return durationFrames[static_cast<size_t>(s)] > 0; }
    // The requested speed if available; else defaultSpeed if available; else the first
    // available of Fast, Medium, Slow, Variable; else defaultSpeed.
    Speed resolve(Speed requested) const;
};

// Tag 0x80000055 (masked & 3) -> speed: 0 Medium, 1 Fast, 2 Variable, 3 Slow.
Speed decodeSpeedTag(uint32_t v);

// ANIM content. Missing tags take the ANIM defaults (fields 3/2/1/0, speed tag 1 = Fast)
// whatever the crouton TYPE says. durationFrames[s] = (fields * frameCount + 1) / 2.
Timing animTiming(const Crouton&, int frameCount);

// Gradient (ILBM/ALGO) content. Durations are tags 0x57/0x58/0x59/0x56 directly; missing tags
// take the ILBM defaults (0x5a, 0x3c, 0x1e, 0x3c; speed tag 0 = Medium).
Timing fixedTiming(const Crouton&);

// T-bar position (0..511) -> animation frame: min(N - 1, tbar * N / 512).
int animFrameForTBar(int tbar, int frameCount);

}  // namespace vt
