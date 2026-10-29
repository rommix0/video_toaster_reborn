#pragma once
// What the UI watches: the Switcher's operator state and the Program and Preview pictures.
// ProgramLoop publishes both (program_loop.h).
#include <cstdint>
#include <string>

#include "vt/background.h"
#include "vt/raster.h"
#include "vt/switcher.h"
#include "vt/timing.h"

namespace vt::media {

// The Switcher as it stood after tick `tick` (the one just finished; -1 before the first tick).
// `transition` is Switcher::transitionActive(), `chroma` chromaEngaged(), `overlayEngaged`
// overlayEngaged(), `frozen` frozen() (it also goes live by itself when a digital move starts, so it is
// published every tick). `lastRefusal` is the reason of the most recent refused postChecked, kept until
// the next refusal or until a postChecked succeeds; empty = none. `refusalCount` counts every
// refused postChecked since the loop was made, so a repeat of the same refusal is visible too.
struct SwitcherSnapshot {
    vt::Source main = vt::Source::In1, preview = vt::Source::In2, overlay = vt::Source::Bkg;
    int tbar = 0;
    bool autoRunning = false, transition = false, chroma = false, overlayEngaged = false, frozen = false;
    bool hasStill = false;  // a still was written into DV1 or DV2 (SAVE has something to save)
    vt::Speed speed = vt::Speed::Medium;
    std::string effectName;
    vt::EffectColour effectColour, background;
    int64_t tick = -1;
    std::string lastRefusal;
    uint64_t refusalCount = 0;
};

class MonitorSink {   // called on the loop thread; must copy and return quickly
public:
    virtual ~MonitorSink() = default;
    // Once per tick, in tick order, with the Program picture (the one the sinks get) and the
    // picture of the Preview bus's source (no effect applied). Both are valid only during the call.
    virtual void onPictures(int64_t tick, const vt::Frame& program, const vt::Frame& preview) = 0;
};

} // namespace vt::media
