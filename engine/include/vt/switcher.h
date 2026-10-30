#pragma once
// The Switcher's operator model: three buses (Main = Program, Preview, Superimpose), the sources
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
#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "vt/background.h"
#include "vt/effect.h"
#include "vt/framestore.h"
#include "vt/key.h"
#include "vt/raster.h"
#include "vt/sound.h"
#include "vt/timing.h"

namespace vt {

enum class Bus { Main, Preview, Superimpose };
// None is only for the Superimpose bus, which can be empty (Main and Preview always have a source).
enum class Source { In1, In2, In3, In4, DV1, DV2, Bkg, None };
// Auto's direction: Forward runs the T-bar to 511 (Space), Reverse back to 0 (Shift+Space,
// manual PDF 185).
enum class Direction { Forward, Reverse };
// How positioning ends (tools/re/POSITION.md 3 step 5): Space or Return, a left click, the right button.
enum class PositionEnd { KeepAndStop, Finish, BackToTBar };

// The live pictures of the four inputs (null = no picture yet, shown as black). DV1, DV2 and BKG
// are the Switcher's own.
struct SourceFrames {
    std::array<const Frame*, 4> inputs{};
};

// The loaded CG key page as the panel shows it (Switcher::cgPage).
struct CgPageState {
    Source dv = Source::None;  // the DV holding the loaded page; None: no page loaded
    bool up = false;           // Superimpose shows it: on Program at `level`
    int level = 0;             // the dissolve level L, 0..255
    bool mark = false;         // the Superimpose bus's `dv` button shows the "CG" picture
    bool operator==(const CgPageState&) const = default;
};

// The levels an Auto writes for a CG key page (CGKEY.md section 2, cgkey_ref.ramp, verified):
// speed = the value $1f0, on = the page is up (the off-dissolve), tbar = the T-bar 0..511 it starts
// from. N = speed >> 2 steps of (k * floor(0xff80 / N)) >> 8, then the final write (255, or 0 off);
// a part-way T-bar scales the step count as the original does. Where the original divides by zero
// (N = 0, or a part-way start that rounds to no steps) only the final write is left (design 2 D).
std::vector<int> cgRamp(int speed, bool on, int tbar);
// The page's share 0..255 over Main of a pixel of alpha 0..3 at level L 0..255 (HARDWARE GUESS).
int cgMix(int alpha, int level, CgMixLaw law);

class Switcher {
public:
    Switcher();

    // --- buses ---
    // Superimpose is the luma keyer's source bus (KEY.md, design 2 A) and can be empty (None).
    // Selections are live at all times, also during a transition: render reads the current
    // selections, and the swap at T-bar 511 (or Take) exchanges whatever is selected then.
    // DV1 or DV2 selected while the DV store is live makes the bus's previous selection, when it
    // was an input, the DV input (FREEZE.md 3: the bus shows that input through DV, delayed).
    // Refused (false, the reason in *whyNot, the bus kept): None on Main or Preview ("no source");
    // DV2 on Superimpose while the store is frozen or the bus shows neither an input nor live DV
    // ("Superimpose: DV2 alone cannot be keyed"). A Superimpose source other than the one it has
    // turns the keyer on (its polarity kept); the same one again does nothing; None empties the
    // bus and leaves the keyer as it is.
    bool select(Bus, Source, std::string* whyNot = nullptr);
    Source selected(Bus) const;

    // --- keyer (design 2 A) ---
    // Off keeps the last polarity (power-up: off, White) and empties Superimpose; Black and White
    // turn the keyer on with that polarity. The level is the clip level 0..kKeyLevelMax, clamped
    // (power-up 128). While a take is open (Auto running, or the T-bar off 0 with nothing
    // engaged) the effect's routing descriptor may hold them: a mode change is ignored if
    // keyModeLocked, a level change if keyLevelLocked (KEY.md 4: the original reverts it).
    // Under Art Card Key (design 2 C) nothing locks: the level is clamped to 255 and sets the
    // T-bar to 2L; Black/White re-apply the card (keyer on, an empty Superimpose restored); Off
    // also remembers the Superimpose source for the next T-bar move to restore.
    void setKeyMode(KeyMode);
    KeyMode keyMode() const { return !keyOn_ ? KeyMode::Off : keyBlack_ ? KeyMode::Black : KeyMode::White; }
    void setKeyLevel(int l);
    int keyLevel() const { return keyLevel_; }

    // --- effect ---
    // Makes `e` the current effect (a null effect is refused). Any transition in progress is
    // abandoned without swapping (T-bar 0, Auto stopped), its buses left as a return to 0 leaves
    // them (see setTBar), and an engaged ChromaFX or overlay is disengaged. A DV-memory effect (usesDvMemory(): DVE moves and ChromaFX) is refused,
    // returning false with a reason in *whyNot and the current effect kept, while ChromaFX is
    // engaged (a DVE move, that is) or while a DVE move is mid-transition or engaged as an
    // overlay (the Glitch overlays; they share the DV memory, spec 5.8). Cut, dissolve and the
    // wipes are never refused. Ruling 23: a ChromaFX chosen while another is engaged REPLACES
    // it: the old one disengages and the new one starts disengaged (T-bar 0, frame counter
    // reset). The effect colour becomes e's own and the speed button e's default
    // (timing().defaultSpeed, tag 0x55: the Switcher sets $1f4 from it when a crouton is
    // loaded); setSpeed then changes it.
    // Art Card Key (isArtCardKey, design 2 C) turns the keyer on, remembers the Superimpose source
    // (an empty bus gets the remembered one back), clamps L to 255 and puts the T-bar at 2L.
    // Loading anything else after it turns the keyer off and empties Superimpose.
    bool setEffect(std::shared_ptr<const Effect> e, std::string* whyNot = nullptr);
    const Effect& effect() const { return *effect_; }

    // The current effect's matte colour, its "Effect Color" (Ruling 26): effect().effectColour()
    // until overridden here (the UI's Effect Color pop-up); setEffect resets it to the new
    // effect's own. An effect without one (no tag 0x80000023/0x24) leaves the colour generator
    // as it is (switcher.main 0xe723e), so effectColour() is then the BKG source's colour
    // (setBackground), following it; effectHasOwnColour() tells the two apart.
    void setEffectColour(EffectColour c) { effectColour_ = c; }
    EffectColour effectColour() const { return effectColour_.value_or(bkg_); }
    bool effectHasOwnColour() const { return effectColour_.has_value(); }

    // --- speed buttons, hardware guesses ---
    // The operator's S/M/F/V button. It is what renders resolve (effect().timing().resolve) and
    // pass on as RenderCtx::speed, e.g. a cycling ChromaFX's cycle rate; autoTransition sets
    // it too. A running Auto keeps the rate it started with.
    void setSpeed(Speed s) { speed_ = s; }
    Speed speed() const { return speed_; }
    // Passed to every effect render as RenderCtx::guesses.
    void setGuesses(const HardwareGuesses& g) { guesses_ = g; }
    const HardwareGuesses& guesses() const { return guesses_; }

    // --- T-bar, Auto, Take ---
    // A digital move starting from idle (FREEZE.md 5): with the store frozen and no bus showing
    // DV1/DV2 the store goes live (stills kept); then, live, the moving bus's input becomes the
    // DV input: A: Main, B: Preview; a flying key moves its own picture instead (fly on: Preview's
    // input; fly off and two-phase: Superimpose's).
    // pos is clamped to 0..511. A manual move to a different position stops a running Auto.
    // Ordinary effects: reaching 511 swaps Main and Preview at once and the T-bar reads 0 again
    // (the next render shows the new Main plainly); returning to 0 mid-way swaps nothing.
    // With a key up the effect's routing descriptor decides (design 2 B, keyMoveFor), from the
    // keyer as it is at each step:
    //  * drops the key (also every non-digital move, overlay and ChromaFX): the keyer turns off
    //    at take start; 511 swaps, and 511 or the return to 0 empties Superimpose;
    //  * fly on (needs only the keyer on): 511 does not swap but puts Preview's source on
    //    Superimpose; the return to 0 empties Superimpose;
    //  * fly off (needs a Superimpose source too, else it is a plain move): 511 or the return to
    //    0 puts the Superimpose source on Preview and empties Superimpose, without a swap;
    //  * two-phase (the same need): 511 changes no bus; the return to 0 is as fly off.
    // Main is never changed by a flying move, and the keyer stays on.
    // The engine's T-bar reads 0 after a hand-driven 511, so the UI must reset its T-bar widget
    // then (otherwise its next setTBar(510) starts a new transition).
    // ChromaFX: leaving 0 engages it (see render); 511 does not swap; returning to 0
    // disengages a Transition-mode crouton (a Filter-mode one stays engaged).
    // Overlays (effect().isOverlay(), Rulings 27, 32): leaving 0 engages it; 511 does not swap;
    // returning to 0 ends it.
    // Art Card Key: the T-bar is the clip control and never starts a transition: L = pos / 2,
    // the keyer on, an empty Superimpose restored; 511 does not swap. Auto does nothing.
    // The first move of a drag (the first since releaseTBar) grabs the T-bar: a Positionable
    // effect's position goes back to its centred defaults, without a stop, and positioning ends.
    void setTBar(int pos);
    int tbar() const { return cgPage_ ? cgTBar_ : tbar_; }  // a loaded CG key page's own T-bar
    // The hand lets go of the T-bar (the drag ends). For a Positionable effect (positionRange()) whose
    // T-bar this drag moved (tools/re/POSITION.md 3): let go off 0 and 511 with the take open and no
    // Auto, positioning starts; let go at 0 or 511 (the take has ended), a kept offset is baked
    // there (0xb7054) at the end where its divu overflows (511, or 0 for a flags != 0 effect) and
    // dropped at the other. Otherwise nothing.
    void releaseTBar();

    // --- positioning (the Positionable moves, POSITION.md) ---
    // While positioning, the mouse moves the effect's centre at the current T-bar position.
    bool positioning() const { return positioning_; }
    // The mouse moved dx, dy counts (right / down +): each anchor moves by algoMouseStep and is
    // clamped to the effect's range (Y first, 0x5e2e8). Ignored unless positioning.
    void movePosition(int dx, int dy);
    // Ignored unless positioning. KeepAndStop (Space, Return) bakes the position at the T-bar
    // (0xb7054), sets the stop and puts the T-bar there (stop >> 7, kept within 1..510); the take
    // stays open. Finish (a left click) bakes and ends the take as at 511, for all four effects (the
    // original may abort Expand and Wipe Out instead, POSITION.md 10). BackToTBar (the right
    // button) makes the next T-bar moves part of the same drag, the offset kept.
    // The position lasts with the loaded effect, across takes, until the next grab or setEffect. A
    // forward Auto from T-bar 0 with a stop holds there: Auto ends with the T-bar at stop >> 7
    // (kept within 1..510), the take open; the next Auto finishes it. take(), autoTransition() and
    // setEffect() end positioning too.
    void endPositioning(PositionEnd);
    const EffectPosition& position() const { return position_; }
    // Sets the speed button to `speed` and runs the T-bar from where it is to 511 (Forward) or
    // to 0 (Reverse) at the constant rate of effect().timing().durationTicks[resolve(speed)]
    // ticks for the full 0..511 trip (the trip from 0 takes exactly that many tick()s; from
    // further along it takes the remaining fraction, rounded up). Forward does nothing if the
    // T-bar already is at 511 (an engaged ChromaFX or overlay), Reverse if it is at 0 and Auto
    // is not running. Called while Auto runs, it carries on from the exact position (same
    // direction: no change but the rate; the other direction: turns round).
    // While it runs the T-bar reads round(511 * elapsed / total) (never 511 or 0 before the
    // end), and an effect with animation frames is rendered at its exact frame
    // elapsed / fieldsPerFrame (RenderCtx::animFrame), so each frame is shown for exactly
    // fieldsPerFrame ticks and none is skipped or repeated. A hand-driven T-bar maps to frames
    // by animFrameForTBar instead.
    // Forward ends as a T-bar run to 511 does (swap, or an engaged ChromaFX or overlay stays
    // at 511); Reverse ends as a return to 0 does: no swap, an overlay ends, a Transition-mode
    // ChromaFX disengages (a Filter-mode one stays engaged).
    void autoTransition(Speed speed, Direction dir = Direction::Forward);
    bool autoRunning() const { return auto_ || cgAuto_; }  // also a CG key page's dissolve
    // True from the moment the T-bar leaves 0 or Auto starts until the transition finishes (or,
    // for ChromaFX and overlays, until it is disengaged).
    bool transitionActive() const { return auto_ || tbar_ > 0 || engaged_; }
    bool chromaEngaged() const { return engaged_ && effect_->isChromaFx(); }
    bool overlayEngaged() const { return engaged_ && effect_->isOverlay(); }
    // Cut: swaps the Main and Preview selections; if Preview was Main and Superimpose has a source,
    // that source goes to Preview, and Superimpose is emptied (design 2 B, KEY.md 4; the keyer mode
    // stays). Ends any transition (T-bar 0, Auto stopped, ChromaFX disengaged) and drops a sound
    // pendingSound() has not handed over. On an engaged overlay it only ends the overlay, without
    // a cut (overlays never switch sources), and empties Superimpose as the return to 0 does
    // (C7: an overlay drops the key). Under Art Card Key the cut keeps the card: with the keyer on Superimpose gets
    // the card's source back, and the T-bar stays at 2L.
    void take();

    // --- background (BKG source) ---
    void setBackground(BkgColour c) { bkg_ = EffectColour::of(c); }
    // Tag 0x80000024 hardware word (customBkgColour); bit 15 set selects Snow.
    void setCustomBackground(uint32_t word) { bkg_ = EffectColour::customWord(word); }
    EffectColour background() const { return bkg_; }

    // --- DV store: DV1 and DV2 (design B) ---
    // The store is live (DV1/DV2 on a bus show the DV input, guesses().digitalDelay ticks late)
    // or frozen (they show their stills). It starts live with both stills black.
    bool frozen() const { return frozen_; }
    int dvInput() const { return dvInput_; }  // 0..3, the one DV input (starts 0)
    const Frame& still(Source dv) const;      // DV1 or DV2 (black until written)
    // FREEZE (true) / LIVE (false). Refused with "finish the effect first" while
    // transitionActive() && !overlayEngaged(). Freezing writes the live DV picture (dvInput's
    // delayed frame) into DV2 if any bus shows DV2, else DV1.
    bool setFrozen(bool frozen, std::string* whyNot = nullptr);
    // Grab input 0..3 into DV1: freezes if live, then digitalDelay ticks later the input's frame
    // goes into DV1 and Main selects DV1 (FREEZE.md 3). Same refusal as setFrozen. The original
    // waits out the 4 fields before it reads another button; here a take or a transition that
    // starts in the wait leaves the buses and the freeze to it: the frame still goes into DV1.
    bool grab(int input, std::string* whyNot = nullptr);
    bool grabPending() const { return grabPending_; }
    // A framestore loaded (FREEZE.md 4): refused with "a transition is running: finish it first"
    // while auto or the T-bar > 0 with nothing engaged; disengages an engaged ChromaFX; buses
    // showing live DV go back to In(dvInput); target = DV2 if Main or Superimpose shows DV1, DV1 if
    // either shows DV2, else DV1 if Preview shows DV2, else DV2; the still is written there, the
    // store frozen, the target selected on Preview.
    bool loadStill(const Frame& still, std::string* whyNot = nullptr);
    // SAVE's source: the DV on Preview, else on Main, else DV1. nullopt (and "SAVE: no still in
    // DV1/DV2") if neither still was ever written.
    std::optional<Source> saveSource(std::string* whyNot = nullptr) const;
    bool hasStill() const { return written_[0] || written_[1]; }  // saveSource has a still
    // The f / l keys: refused ("f / l: a CG key page is loaded") while a CG key page is loaded (the
    // page's handler refuses message 0xf, CGKEY.md section 3); otherwise setFrozen. The FREEZE and
    // LIVE buttons call setFrozen, which a page does not intercept.
    bool freezeKey(bool frozen, std::string* whyNot = nullptr);

    // --- ToasterCG key pages (docs/2026-10-30-cg-keys-design.md, tools/re/CGKEY.md) ---
    // While a page is loaded it is the loaded crouton: Auto, the T-bar, Take, the keyer buttons and
    // the Superimpose bus act on it as section 2 C-E say, and tbar() / autoRunning() are its own.
    // Loading: clicking the page that is loaded again (same path) puts its DV on Preview; with cutOn
    // (a double click) it is loaded if it is not already, then cut on. Otherwise the previous page is
    // cleared, an engaged ChromaFX disengages, and the effect becomes the plain built-in dissolve as
    // setEffect does it (the page replaces the loaded crouton: a take in progress is abandoned, a
    // held Art Card Key ends); live DV on a bus goes back to its input; Superimpose is emptied if it
    // and Main show different DVs; then the fill goes into DV1/DV2 by loadStill's rule, the store
    // frozen. Nothing else on the buses or the keyer changes. The speed button goes to the page's
    // default. Refused only for a page that is not a decoded one.
    bool loadCgPage(std::shared_ptr<const CgKeyPage>, bool cutOn = false, std::string* whyNot = nullptr);
    // Another crouton selected (setEffect and loadStill do it themselves; a control crouton calls
    // it): as the end of an off-dissolve, a cut that empties Superimpose (Main kept, Preview gets the
    // page's DV if it was Main), the alpha and the mark gone; the picture stays in the DV.
    void clearCgPage();
    CgPageState cgPage() const;

    // One 59.94 fps frame (call it after that frame's render). Advances the clock, a running
    // Auto, (while engaged) the ChromaFX / overlay frame counter and a pending grab's wait.
    void tick();

    // Program = effect(tbar, A = Main source, B = Preview source). Rules:
    //  * Art Card Key: Main keyed over Preview by the Superimpose source's luma, v = Black ? L :
    //    255 - L (lumaKey), with the keyer on and a Superimpose source; else Main;
    //  * no transition (T-bar 0, Auto not running, nothing engaged): Program = A, or, with the keyer on
    //    and a Superimpose source other than Main: the key (L 0: Superimpose alone; 257: A alone;
    //    else lumaKey by the clip value, keyClip, with the Superimpose picture as fill and signal);
    //  * ordinary effect: the effect at the current T-bar position (stateless per position);
    //  * a flying key (see setTBar): the effect, given a = b = Preview (fly on), a = b =
    //    Superimpose (fly off) or a = Superimpose, b = Main (two-phase), with RenderCtx::keyFill
    //    the polarity's colour (Black or White, bkgColour), keyed by its own luma over Main as
    //    the idle key is (two-phase from T-bar 256: over the Superimpose source). Program is the
    //    picture under the key when the keyed source is that same source;
    //  * engaged ChromaFX: the effect's output with B = Preview source at the T-bar position
    //    (Program shows the recoloured Preview), frameNumber = ticks since engagement;
    //  * engaged overlay (Ruling 32), A = Main source:
    //      - animated matte: B = a frame of effectColour() (Snow: noise), so the matte shows the
    //        effect colour over Main;
    //      - DVE move (the Glitch overlays): B = Main as well (Main distorted);
    //      - gradient (Split1/2/3 !): B = Preview source at the T-bar position, a split screen
    //        held where the operator leaves the T-bar.
    //    Animated ones loop: frame (ticks since engagement / fieldsPerFrame) mod frameCount at
    //    the speed button (RenderCtx::animFrame), so frame 0 again after frameCount x
    //    fieldsPerFrame ticks.
    // RenderCtx::background is effectColour().rgb() (black for Snow); with a Snow effect colour
    // RenderCtx::generator is this tick's noise frame, the same one an overlay's B gets.
    // RenderCtx::colour is effectColour(), RenderCtx::take the take number (it changes when a
    // transition starts from idle and on setEffect), and during a forward Auto started at T-bar 0
    // RenderCtx::autoStep is the elapsed ticks + 1 (1..autoSteps, the Auto's total ticks; the
    // ALGO moves step by it, ALGO.md 15.1, Ruling 7). Never reads or writes `program` before it
    // is finished, so it may alias an input frame.
    // `program`'s pixel buffer is swapped with an internal one on every call (that is what keeps
    // steady-state renders allocation-free), so program.px.data() alternates between two
    // buffers: do not keep a pointer into program.px across renders.
    // Before composing, render writes a due grab into DV1 and records the DV input's frame in
    // the DV history (what live DV1/DV2 show, delayed); while the effect is a digital move
    // (Effect::digitalMove()), also the inputs on Main, Preview and Superimpose. A digital move's
    // RenderCtx::digitalA / digitalB are those buses' inputs, delayed, while the store is live.
    void render(const SourceFrames&, Frame& program) const;
    // The picture a bus would show for source `s` (an input, DV1/DV2, or BKG: its colour or this
    // tick's Snow), with no effect applied: what the Preview monitor shows. A missing input is
    // black. Same scratch rules as render (serialised with it); `out` may alias an input frame.
    // It reads the DV history but never records.
    void renderSource(Source s, const SourceFrames&, Frame& out) const;

    // The current effect's sound, once, when a transition starts; nullptr otherwise (and after
    // a Take). The pointer shares ownership of the effect, so the sound stays valid however
    // long the caller keeps it, also after setEffect() released the effect.
    std::shared_ptr<const Sound> pendingSound();

private:
    void beginTransitionIfIdle();
    void finishAt511();
    void arriveAt0();
    void engage();
    void disengage();
    KeyMove keyMove() const;  // the effect's key move: Drops unless a digital move (C2)
    KeyMove flight() const;   // what this take does with the key now: Drops if nothing flies
    bool takeOpen() const;    // the keyer locks apply (C5)
    void applyCard();         // Art Card Key, KEY.md 6 steps 3-4: Superimpose, L <= 255, T-bar 2L
    const Frame& sourceFrame(Source, const SourceFrames&, bool& bkgBuilt, bool& blackBuilt) const;
    void record(int input, const SourceFrames&) const;
    // `input`'s picture through DV: the newest recorded frame with tick <= now - delay, else
    // the oldest recorded one; null (black) if none.
    const Frame* delayed(int input) const;
    void writeStill(int dv, const Frame* f) const;  // dv 0 = DV1, 1 = DV2; null = black
    Source dvTarget() const;  // where a framestore or CG key page loads (FREEZE.md 4)
    bool cgUp() const { return cgPage_ && sel_[2] == cgDv_; }  // the page is on Superimpose
    void cgCutOn();            // 0x123ad6: the page on Superimpose at full level
    void cgDrop();             // the keyer buttons, another Superimpose source: the page cuts off
    void cgShow();             // 0x123d62 when no page bitmap is shown: the page's DV on Superimpose
    void cgFinish();           // the Auto's last write was held: its end (CGKEY.md section 2)
    bool staysEngaged() const { return effect_->isChromaFx() || effect_->isOverlay(); }

    std::array<Source, 3> sel_{Source::In1, Source::In2, Source::None};  // Main, Preview, Superimpose
    bool keyOn_ = false, keyBlack_ = false;
    int keyLevel_ = 128;
    Source cardSaved_ = Source::None;  // the Superimpose source Art Card Key restores
    std::shared_ptr<const Effect> effect_;
    int tbar_ = 0;
    bool auto_ = false;
    int64_t autoAcc_ = 0;  // T-bar position * autoDur_ (Auto's exact position; 511 per tick)
    int autoDur_ = 1;      // ticks for the full 0..511 trip
    int autoFpf_ = 0;      // fields (ticks) per animation frame at Auto's speed; 0 = no frames
    Direction autoDir_ = Direction::Forward;
    bool autoFrom0_ = false;  // this Auto runs forward from T-bar 0 (RenderCtx::autoStep); only while auto_
    uint64_t take_ = 0;       // RenderCtx::take: bumped when a transition starts from idle, setEffect
    EffectPosition position_;  // RenderCtx::position: the Positionable effect's, reset by a grab or setEffect
    bool positioning_ = false;
    bool dragging_ = false;    // a drag has grabbed the T-bar (setTBar) and not let go (releaseTBar)
    int handTBar_ = 0;         // the drag's last setTBar position
    Speed speed_ = Speed::Medium;
    HardwareGuesses guesses_;
    bool engaged_ = false;       // a ChromaFX or overlay is engaged
    uint32_t engagedTicks_ = 0;  // ticks since engagement
    bool soundPending_ = false;
    uint32_t tickCount_ = 0;
    EffectColour bkg_;           // the BKG source
    std::optional<EffectColour> effectColour_;  // the effect's matte colour; none: BKG's
    mutable Frame bkgScratch_, blackScratch_, outScratch_, keyScratch_;  // render's working frames
    mutable Frame colourScratch_;      // the effect colour as a picture (an overlay's B, Snow)
    mutable bool colourValid_ = false;  // colourScratch_ holds colourBuilt_ (never for Snow)
    mutable EffectColour colourBuilt_;
    // The DV store. The stills, written_ and the grab state are mutable because render writes a
    // due grab into DV1.
    bool frozen_ = false;
    int dvInput_ = 0;
    mutable std::array<Frame, 2> stills_;  // DV1, DV2
    mutable std::array<bool, 2> written_{};
    mutable bool grabPending_ = false;
    int grabInput_ = 0;
    int grabWait_ = 0;  // ticks until the grab is due
    bool grabSelects_ = false;  // the due grab puts DV1 on Main and freezes; a take or transition clears it
    // The DV history: per input, a ring of delay + 1 frames and the ticks they were recorded at
    // (-1 = empty), allocated when the input is first recorded. Recording here is the exception
    // to "render reads only" (with the due grab above): the lag needs the past frames.
    mutable std::array<std::vector<Frame>, 4> hist_;
    mutable std::array<std::vector<int64_t>, 4> histTicks_;
    // The loaded CG key page (CGKEY.md names: on flag 0x124434, $3e6, $1ec). Null: none.
    std::shared_ptr<const CgKeyPage> cgPage_;
    Source cgDv_ = Source::None;  // the DV holding it
    bool cgOn_ = false;           // the page is up: the next Auto dissolves it off
    bool cgShown_ = false;        // its alpha is on the Amiga display ($3e6 != 0)
    bool cgMark_ = false;         // the CG picture on the Superimpose DV button
    int cgLevel_ = 0;             // the dissolve level L last written
    int cgTBar_ = 0;              // the T-bar while a page is loaded
    bool cgAuto_ = false;         // a page dissolve runs
    bool cgAutoOff_ = false;      // ... and it is the off-dissolve
    std::vector<int> cgRamp_;     // its levels (cgRamp)
    size_t cgStep_ = 0;           // the level shown
    int cgWait_ = 0;              // ticks until the next level
};

}  // namespace vt
