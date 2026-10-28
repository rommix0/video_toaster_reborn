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
    // Effect length in Toaster frames (29.97 fps, what the manual's Length shows) at each
    // speed. 0 means the speed is unavailable.
    std::array<int, 4> durationFrames{};
    // Effect length in Switcher ticks (one tick = one 59.94 fps output frame = one Toaster
    // field) at each speed: what Auto takes (Ruling 25). ANIM content: fieldsPerFrame x frame
    // count, so every animation frame is shown for exactly fieldsPerFrame ticks; ALGO content:
    // the speed tags (algoTiming); everything else: 2 x durationFrames. Positive exactly when
    // durationFrames is.
    std::array<int, 4> durationTicks{};
    Speed defaultSpeed = Speed::Medium;

    bool available(Speed s) const { return durationFrames[static_cast<size_t>(s)] > 0; }
    // The requested speed if available; else defaultSpeed if available; else the first
    // available of Fast, Medium, Slow, Variable; else defaultSpeed.
    Speed resolve(Speed requested) const;
};

// Tag 0x80000055 (masked & 3) -> speed: 0 Medium, 1 Fast, 2 Variable, 3 Slow.
Speed decodeSpeedTag(uint32_t v);

// ANIM content. Missing tags take the ANIM defaults (fields 3/2/1/0, speed tag 1 = Fast)
// whatever the crouton TYPE says. durationFrames[s] = (fields * frameCount + 1) / 2
// (switcher.main 0xe6548), durationTicks[s] = fields * frameCount.
Timing animTiming(const Crouton&, int frameCount);

// Gradient (ILBM) content. durationFrames are tags 0x57/0x58/0x59/0x56 directly (missing
// tags take the ILBM defaults 0x5a, 0x3c, 0x1e, 0x3c; speed tag 0 = Medium); durationTicks are
// twice those.
Timing fixedTiming(const Crouton&);

// ALGO content. durationTicks = tags 0x57/0x58/0x59/0x56 (defaults 0x5a, 0x3c, 0x1e, 0x3c;
// ALGO.md 3: the speed tags are fields), durationFrames = (ticks + 1) / 2, fieldsPerFrame 0,
// speed tag 0x55 default 0 (Medium).
Timing algoTiming(const Crouton&);

// T-bar position (clamped to 0..511) -> animation frame: min(N - 1, tbar * N / 512); 0 when
// N <= 0. The Switcher uses it
// for a hand-driven T-bar; during Auto it gives the effect the exact frame instead
// (RenderCtx::animFrame).
int animFrameForTBar(int tbar, int frameCount);

}  // namespace vt
