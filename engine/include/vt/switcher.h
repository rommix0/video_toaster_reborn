#pragma once
// The Switcher's operator model: three buses (Main = Program, Preview, Overlay), the sources
// they select, the effect, the T-bar, Auto and Take. It drives the effects of vt/effect.h and
// produces the Program picture; the Qt app sits on top of it.
//
// Clock (Ruling 25): one tick() is one 59.94 fps output frame, i.e. one Toaster field. For every
// output frame the caller renders first and ticks after:
//     sw.render(sources, program);  /* show or record program */  sw.tick();
// so the first frame after autoTransition() shows the T-bar at 0 (animation frame 0), and the last
// frame of a transition is rendered before the tick that finishes it (and swaps Main and Preview).
//
// Threading: single-threaded. Every call must be serialised by the caller, including render():
// it is const but works in member scratch frames, so two renders must not overlap either.
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
    // Selections are live at all times, also during a transition: render reads the current
    // selections, and the swap at T-bar 511 (or Take) exchanges whatever is selected then.
    void select(Bus, Source);
    Source selected(Bus) const;

    // --- effect ---
    // Makes `e` the current effect (a null effect is refused). Any transition in progress is
    // abandoned without swapping (T-bar 0, Auto stopped) and an engaged ChromaFX is disengaged.
    // A DV-memory effect (usesDvMemory(): DVE moves and ChromaFX) is refused, returning false
    // with a reason in *whyNot and the current effect kept, while ChromaFX is engaged (a DVE
    // move, that is) or while a DVE move is mid-transition (they share the DV memory, spec
    // 5.8). Cut, dissolve and the wipes are never refused. Ruling 23: a ChromaFX chosen while
    // another is engaged REPLACES it: the old one disengages and the new one starts
    // disengaged (T-bar 0, frame counter reset). The effect colour becomes e's own.
    bool setEffect(std::shared_ptr<const Effect> e, std::string* whyNot = nullptr);
    const Effect& effect() const { return *effect_; }

    // The current effect's matte colour, its "Effect Color" (Ruling 26): effect().effectColour()
    // until overridden here (the UI's Effect Color pop-up); setEffect resets it to the new
    // effect's own. Independent of the BKG source's colour (setBackground).
    void setEffectColour(EffectColour c) { effectColour_ = c; }
    EffectColour effectColour() const { return effectColour_; }

    // --- T-bar, Auto, Take ---
    // pos is clamped to 0..511. A manual move to a different position stops a running Auto.
    // Ordinary effects: reaching 511 swaps Main and Preview at once and the T-bar reads 0 again
    // (the next render shows the new Main plainly); returning to 0 mid-way swaps nothing.
    // The engine's T-bar reads 0 after a hand-driven 511, so the UI must reset its T-bar widget
    // then (otherwise its next setTBar(510) starts a new transition).
    // ChromaFX: leaving 0 engages it (see render); 511 does not swap; returning to 0
    // disengages a Transition-mode crouton (a Filter-mode one stays engaged).
    void setTBar(int pos);
    int tbar() const { return tbar_; }
    // Runs the T-bar from where it is to 511 at the constant rate of
    // effect().timing().durationTicks[resolve(speed)] ticks for the full 0..511 trip (the
    // trip from 0 takes exactly that many tick()s; from further along it takes the remaining
    // fraction, rounded up). Does nothing if the T-bar already is at 511 (ChromaFX).
    // While it runs the T-bar reads round(511 * elapsed / total) (never 511 before the end), and
    // an effect with animation frames is rendered at its exact frame elapsed / fieldsPerFrame
    // (RenderCtx::animFrame), so each frame is shown for exactly fieldsPerFrame ticks and none
    // is skipped or repeated. A hand-driven T-bar maps to frames by animFrameForTBar instead.
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
    void setBackground(BkgColour c) { bkg_ = EffectColour::of(c); }
    // Tag 0x80000024 hardware word (customBkgColour); bit 15 set selects Snow.
    void setCustomBackground(uint32_t word) { bkg_ = EffectColour::customWord(word); }
    EffectColour background() const { return bkg_; }

    // One 59.94 fps frame (call it after that frame's render). Advances the clock, a running
    // Auto and (while engaged) the ChromaFX frame counter.
    void tick();

    // Program = effect(tbar, A = Main source, B = Preview source). Rules:
    //  * no transition (T-bar 0, ChromaFX not engaged): Program = A;
    //  * ordinary effect: the effect at the current T-bar position (stateless per position);
    //  * engaged ChromaFX: the effect's output with B = Preview source at the T-bar position
    //    (Program shows the recoloured Preview), frameNumber = ticks since engagement.
    // RenderCtx::background is effectColour().rgb() (black for Snow), whatever BKG shows. Never
    // reads or writes `program` before it is finished, so it may alias an input frame.
    void render(const SourceFrames&, Frame& program) const;

    // The current effect's sound, once, when a transition starts; nullptr otherwise. The pointer
    // is valid only until the next setEffect().
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
    int64_t autoAcc_ = 0;  // T-bar position * autoDur_ (Auto's exact position; 511 per tick)
    int autoDur_ = 1;      // ticks for the full 0..511 trip
    int autoFpf_ = 0;      // fields (ticks) per animation frame at Auto's speed; 0 = no frames
    bool chromaEngaged_ = false;
    uint32_t chromaFrames_ = 0;  // ticks since engagement
    bool soundPending_ = false;
    uint32_t tickCount_ = 0;
    EffectColour bkg_;           // the BKG source
    EffectColour effectColour_;  // the effect's matte colour
    mutable Frame bkgScratch_, blackScratch_, outScratch_;  // render's working frames
    Frame dv_;  // DV1 and DV2 stay black until freeze / framestores exist (spec 3)
};

}  // namespace vt
