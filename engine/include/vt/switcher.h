#pragma once
// The Switcher's operator model: three buses (Main = Program, Preview, Overlay), the sources
// they select, the effect, the T-bar, Auto and Take. It drives the effects of vt/effect.h and
// produces the Program picture; the Qt app sits on top of it.
//
// Threading: single-threaded. Every call (including render, which reads the whole state) must
// be serialised by the caller.
#include <array>
#include <cstdint>
#include <memory>
#include <string>

#include "vt/background.h"
#include "vt/effect.h"
#include "vt/raster.h"
#include "vt/sound.h"
#include "vt/timing.h"

namespace vt {

enum class Bus { Main, Preview, Overlay };
enum class Source { In1, In2, In3, In4, DV1, DV2, Bkg };

// The live pictures of the four inputs (null = no picture yet, shown as black). DV1, DV2 and BKG
// are the Switcher's own.
struct SourceFrames {
    std::array<const Frame*, 4> inputs{};
};

class Switcher {
public:
    Switcher();

    // --- buses ---
    // Overlay is present and selectable, but version 1 has no keying (spec 3): render ignores it.
    void select(Bus, Source);
    Source selected(Bus) const;

    // --- effect ---
    // Makes `e` the current effect (a null effect is refused). Any transition in progress is
    // abandoned without swapping (T-bar 0, Auto stopped) and an engaged ChromaFX is disengaged.
    // A DV-memory effect (usesDvMemory(): DVE moves and ChromaFX) is refused, returning false
    // with a reason in *whyNot and the current effect kept, while ChromaFX is engaged or while
    // a DVE move is mid-transition (they share the DV memory, spec 5.8). Cut, dissolve and
    // the wipes are never refused.
    bool setEffect(std::shared_ptr<const Effect> e, std::string* whyNot = nullptr);
    const Effect& effect() const { return *effect_; }

    // --- T-bar, Auto, Take ---
    // pos is clamped to 0..511. A manual move to a different position stops a running Auto.
    // Ordinary effects: reaching 511 swaps Main and Preview at once and the T-bar reads 0 again
    // (the next render shows the new Main plainly); returning to 0 mid-way swaps nothing.
    // ChromaFX: leaving 0 engages it (see render); 511 does not swap; returning to 0
    // disengages a Transition-mode crouton (a Filter-mode one stays engaged).
    void setTBar(int pos);
    int tbar() const { return tbar_; }
    // Runs the T-bar from where it is to 511 at the constant rate of
    // effect().timing().durationFrames[resolve(speed)] ticks for the full 0..511 trip (the
    // trip from 0 takes exactly that many tick()s; from further along it takes the remaining
    // fraction, rounded up). Does nothing if the T-bar already is at 511 (ChromaFX).
    void autoTransition(Speed);
    bool autoRunning() const { return auto_; }
    // True from the moment the T-bar leaves 0 or Auto starts until the transition finishes (or,
    // for ChromaFX, until it is disengaged).
    bool transitionActive() const { return auto_ || tbar_ > 0 || chromaEngaged_; }
    bool chromaEngaged() const { return chromaEngaged_; }
    // Cut: swaps the Main and Preview selections. Ends any transition (T-bar 0, Auto stopped,
    // ChromaFX disengaged).
    void take();

    // --- background (BKG source) ---
    void setBackground(BkgColour);
    // Tag 0x80000024 hardware word (customBkgColour); bit 15 set selects Snow.
    void setCustomBackground(uint32_t word);

    // One 59.94 fps frame. Advances the clock, a running Auto and (while engaged) the ChromaFX
    // frame counter.
    void tick();

    // Program = effect(tbar, A = Main source, B = Preview source). Rules:
    //  * no transition (T-bar 0, ChromaFX not engaged): Program = A;
    //  * ordinary effect: the effect at the current T-bar position (stateless per position);
    //  * engaged ChromaFX: the effect's output with B = Preview source at the T-bar position
    //    (Program shows the recoloured Preview), frameNumber = ticks since engagement.
    // RenderCtx::background is the BKG colour (black when BKG is Snow). Never reads or writes
    // `program` before it is finished, so it may alias an input frame.
    void render(const SourceFrames&, Frame& program) const;

    // The current effect's sound, once, when a transition starts; nullptr otherwise.
    const Sound* pendingSound();

private:
    void beginTransitionIfIdle();
    void finishAt511();
    void disengage();
    void fillSource(Source, const SourceFrames&, Frame& scratch, const Frame*& out) const;

    std::array<Source, 3> sel_{Source::In1, Source::In2, Source::Bkg};
    std::shared_ptr<const Effect> effect_;
    int tbar_ = 0;
    bool auto_ = false;
    int64_t autoAcc_ = 0;  // T-bar position * autoDur_ (Auto's exact position)
    int autoDur_ = 1;
    bool chromaEngaged_ = false;
    uint32_t chromaFrames_ = 0;  // ticks since engagement
    bool soundPending_ = false;
    uint32_t tickCount_ = 0;
    BkgColour bkg_ = BkgColour::Black;
    bool bkgCustom_ = false;
    uint32_t bkgWord_ = 0;
    Frame dv_;  // DV1 and DV2 stay black until freeze / framestores exist (spec 3)
};

}  // namespace vt
