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
// overlayEngaged(). `lastRefusal` is the reason of the most recent refused postChecked, kept until
// the next refusal or until a postChecked succeeds; empty = none.
struct SwitcherSnapshot {
    vt::Source main = vt::Source::In1, preview = vt::Source::In2, overlay = vt::Source::Bkg;
    int tbar = 0;
    bool autoRunning = false, transition = false, chroma = false, overlayEngaged = false;
    vt::Speed speed = vt::Speed::Medium;
    std::string effectName;
    vt::EffectColour effectColour, background;
    int64_t tick = -1;
    std::string lastRefusal;
};

class MonitorSink {   // called on the loop thread; must copy and return quickly
public:
    virtual ~MonitorSink() = default;
    // Once per tick, in tick order, with the Program picture (the one the sinks get) and the
    // picture of the Preview bus's source (no effect applied). Both are valid only during the call.
    virtual void onPictures(int64_t tick, const vt::Frame& program, const vt::Frame& preview) = 0;
};

} // namespace vt::media
