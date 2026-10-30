#include "vt/switcher.h"

#include <algorithm>
#include <climits>
#include <utility>

#include "vt/algo.h"

namespace vt {
namespace {

constexpr int kTBarEnd = 511;
constexpr int kCardLevelMax = 255;  // Art Card Key clamps L (KEY.md 6 step 4)
constexpr Rgba8 kBlack{0, 0, 0, 255};

size_t busIndex(Bus b) { return static_cast<size_t>(b); }
bool isInput(Source s) { return s <= Source::In4; }
bool isDv(Source s) { return s == Source::DV1 || s == Source::DV2; }
int dvIndex(Source s) { return s == Source::DV2 ? 1 : 0; }
int digitalDelay(const HardwareGuesses& g) { return std::clamp(g.digitalDelay, 0, 8); }
constexpr const char* kFinishEffect = "finish the effect first";

// What an engaged overlay lays over Main (Ruling 32).
enum class OverlayB { Colour, Main, Preview };
OverlayB overlayB(const Effect& e) {
    if (e.usesDvMemory()) return OverlayB::Main;  // DVE content: the Glitch overlays distort Main
    const Timing t = e.timing();
    for (int f : t.fieldsPerFrame)
        if (f > 0) return OverlayB::Colour;  // animated matte: the effect colour over Main
    return OverlayB::Preview;                // gradient content: a split screen with Preview
}

}  // namespace

Switcher::Switcher() : effect_(makeDissolve()), effectColour_(effect_->effectColour()) {
    speed_ = effect_->timing().defaultSpeed;
    for (Frame& f : stills_) std::fill(f.px.begin(), f.px.end(), kBlack);
}

bool Switcher::select(Bus b, Source s, std::string* whyNot) {
    auto refuse = [&](const char* why) {
        if (whyNot) *whyNot = why;
        return false;
    };
    Source& cur = sel_[busIndex(b)];
    if (b == Bus::Superimpose) {
        if (s == cur) return true;  // the lit source again: nothing
        // DV2 alone is the plain still unless the store is live and the bus has an input (or live DV) to show.
        if (s == Source::DV2 && (frozen_ || !(isInput(cur) || isDv(cur)))) return refuse("Superimpose: DV2 alone cannot be keyed");
    } else if (s == Source::None) {
        return refuse("no source");
    }
    if (!frozen_ && isDv(s) && isInput(cur)) dvInput_ = static_cast<int>(cur);  // FREEZE.md 3
    cur = s;
    if (b == Bus::Superimpose && s != Source::None) keyOn_ = true;  // keeps the polarity
    return true;
}
Source Switcher::selected(Bus b) const { return sel_[busIndex(b)]; }

void Switcher::setKeyMode(KeyMode m) {
    if (effect_->isArtCardKey()) {
        Source& sup = sel_[busIndex(Bus::Superimpose)];
        keyOn_ = m != KeyMode::Off;
        if (keyOn_) {
            keyBlack_ = m == KeyMode::Black;
            applyCard();
        } else {  // the card ends until the T-bar moves; its source is kept for then
            if (sup != Source::None) cardSaved_ = sup;
            sup = Source::None;
        }
        return;
    }
    if (takeOpen() && keyModeLocked(effect_->routingDescriptor())) return;  // the effect holds it (C5)
    keyOn_ = m != KeyMode::Off;
    if (keyOn_) keyBlack_ = m == KeyMode::Black;
    else sel_[busIndex(Bus::Superimpose)] = Source::None;  // OFF empties the bus
}

void Switcher::setKeyLevel(int l) {
    if (effect_->isArtCardKey()) {
        keyLevel_ = std::clamp(l, 0, kCardLevelMax);
        applyCard();
        return;
    }
    if (takeOpen() && keyLevelLocked(effect_->routingDescriptor())) return;  // the effect holds it (C5)
    keyLevel_ = std::clamp(l, 0, kKeyLevelMax);
}

// C2: only a digital move that is neither an overlay nor a ChromaFX can fly the key.
KeyMove Switcher::keyMove() const {
    const Effect& e = *effect_;
    if (e.digitalMove() == DigitalMove::None || e.isOverlay() || e.isChromaFx()) return KeyMove::Drops;
    return keyMoveFor(e.routingDescriptor());
}

// Decided from the keyer as it is now (C6): a key flies with the keyer on and something to fly.
// Fly on needs no Superimpose source; fly off and two-phase with an empty bus are plain moves.
KeyMove Switcher::flight() const {
    const KeyMove m = keyMove();
    if (!keyOn_ || m == KeyMove::Drops) return KeyMove::Drops;
    if (m != KeyMove::FliesOn && sel_[busIndex(Bus::Superimpose)] == Source::None) return KeyMove::Drops;
    return m;
}

// C5: a take is open while Auto runs or the T-bar is off 0 with nothing engaged; never under the
// Art Card Key (its T-bar is the clip control).
bool Switcher::takeOpen() const { return (auto_ || (tbar_ > 0 && !engaged_)) && !effect_->isArtCardKey(); }

// With the keyer on, a Superimpose source is remembered and an empty bus gets the remembered one
// back; L is clamped to 255 and the T-bar follows it (2L).
void Switcher::applyCard() {
    Source& sup = sel_[busIndex(Bus::Superimpose)];
    if (keyOn_) {
        if (sup != Source::None) cardSaved_ = sup;
        else sup = cardSaved_;
    }
    keyLevel_ = std::min(keyLevel_, kCardLevelMax);
    tbar_ = 2 * keyLevel_;
}

bool Switcher::setEffect(std::shared_ptr<const Effect> e, std::string* whyNot) {
    auto refuse = [&](const char* why) {
        if (whyNot) *whyNot = why;
        return false;
    };
    if (!e) return refuse("no effect given");
    // Ruling 23: a ChromaFX replaces an engaged ChromaFX (the old one disengages below).
    const bool replacingChroma = chromaEngaged() && e->isChromaFx();
    if (e->usesDvMemory() && !replacingChroma) {
        // ChromaFX and the DVE moves share the DV memory (spec 5.8): only one can use it at a time.
        if (chromaEngaged()) return refuse("a ChromaFX effect is engaged: press Take or return the T-bar to 0 first");
        if (effect_->usesDvMemory() && transitionActive())
            return refuse("a DVE move is running: finish it before choosing another DV-memory effect");
    }
    // Abandon whatever was in progress, without a swap: the buses as at a return to 0 (C7), decided
    // by the old effect. Not under Art Card Key: the card holds the T-bar with no take.
    if (transitionActive() && !effect_->isArtCardKey()) arriveAt0();
    auto_ = false;
    tbar_ = 0;
    disengage();
    soundPending_ = false;
    if (effect_->isArtCardKey() && !e->isArtCardKey()) {  // leaving the card ends its key
        keyOn_ = false;
        sel_[busIndex(Bus::Superimpose)] = Source::None;
    }
    effect_ = std::move(e);
    position_ = {};  // the position lasts with the effect
    positioning_ = dragging_ = false;
    ++take_;  // the new effect starts a new take (ALGO.md 15.1: a crouton selected ends the take)
    effectColour_ = effect_->effectColour();
    speed_ = effect_->timing().defaultSpeed;
    if (effect_->isArtCardKey()) {  // design 2 C: the keyer on, the T-bar at 2L
        keyOn_ = true;
        applyCard();
    }
    return true;
}

void Switcher::beginTransitionIfIdle() {
    if (transitionActive()) return;
    soundPending_ = effect_->sound() != nullptr;
    ++take_;
    grabSelects_ = false;  // a pending grab must not switch Main or freeze mid-transition
    if (keyMove() == KeyMove::Drops) keyOn_ = false;  // design 2 B: the key drops at take start
    // FREEZE.md 5 (flag 0x200): a digital move with the store frozen and no DV on a bus sends the
    // store live (the stills are kept), then routes the moving bus's input through DV. With a DV
    // on a bus the freeze stays, and the move moves that still.
    const DigitalMove m = effect_->digitalMove();
    if (m == DigitalMove::None) return;
    if (frozen_ && std::none_of(sel_.begin(), sel_.end(), isDv)) frozen_ = false;
    // A flying key routes the picture it flies instead: Preview's (fly on) or the Superimpose
    // source (fly off, two-phase).
    const KeyMove f = flight();
    const Bus movingBus = f == KeyMove::FliesOn ? Bus::Preview
                          : f != KeyMove::Drops ? Bus::Superimpose
                          : m == DigitalMove::A ? Bus::Main
                                                : Bus::Preview;
    const Source moving = sel_[busIndex(movingBus)];
    if (!frozen_ && isInput(moving)) dvInput_ = static_cast<int>(moving);
}

void Switcher::engage() {
    if (engaged_) return;
    engaged_ = true;
    engagedTicks_ = 0;
}

void Switcher::disengage() {
    engaged_ = false;
    engagedTicks_ = 0;
}

void Switcher::finishAt511() {
    auto_ = false;
    if (staysEngaged()) {
        tbar_ = kTBarEnd;  // stays engaged, no swap (Rulings 21, 27)
        return;
    }
    tbar_ = 0;  // the effect is finished; the T-bar counts as back at 0
    // Design 2 B: a flying key never swaps, and the keyer stays on.
    Source& prev = sel_[busIndex(Bus::Preview)];
    Source& sup = sel_[busIndex(Bus::Superimpose)];
    switch (flight()) {
        case KeyMove::Drops:
            std::swap(sel_[busIndex(Bus::Main)], prev);
            sup = Source::None;  // C7
            break;
        case KeyMove::FliesOn: sup = prev; break;  // Preview is the key
        case KeyMove::FliesOff:                    // the key source sits on Preview, ready to fly back on
            prev = sup;
            sup = Source::None;
            break;
        case KeyMove::TwoPhase: break;  // the buses as at take start
    }
}

// The T-bar is back at 0 (by hand or by a reverse Auto): nothing swaps; an overlay ends; a
// Transition-mode ChromaFX disengages, a Filter-mode one stays engaged. Superimpose is emptied
// (C7, KEY.md 4), its source first going to Preview after fly off and two-phase; a dropped key
// stays off.
void Switcher::arriveAt0() {
    const KeyMove f = flight();
    auto_ = false;
    tbar_ = 0;
    if (!effect_->isChromaFilter()) disengage();
    Source& sup = sel_[busIndex(Bus::Superimpose)];
    if (f == KeyMove::FliesOff || f == KeyMove::TwoPhase) sel_[busIndex(Bus::Preview)] = sup;
    sup = Source::None;
}

void Switcher::setTBar(int pos) {
    pos = std::clamp(pos, 0, kTBarEnd);
    if (pos == tbar_) return;  // also keeps a running Auto going if a UI echoes the T-bar back
    auto_ = false;             // the hand takes over
    // A grab: 0xb6eac resets the program. A grab while positioning is the back-to-T-bar exit,
    // whether or not the UI sent it first (its snapshot can lag a release by a poll).
    if (!std::exchange(dragging_, true) && !positioning_) position_ = {};
    positioning_ = false;
    handTBar_ = pos;
    if (effect_->isArtCardKey()) {  // the clip control: the card is held, no transition starts
        keyOn_ = true;
        keyLevel_ = pos / 2;
        applyCard();
        tbar_ = pos;
        return;
    }
    if (tbar_ == 0 && pos > 0) {
        beginTransitionIfIdle();
        if (staysEngaged()) engage();
    }
    tbar_ = pos;
    if (pos == 0) arriveAt0();
    else if (pos == kTBarEnd && !staysEngaged()) finishAt511();
}

// POSITION.md 3 step 3: the drag (0xb6eac's T-bar loop) ends.
void Switcher::releaseTBar() {
    if (!std::exchange(dragging_, false)) return;
    const auto r = effect_->positionRange();
    if (!r) return;
    if (handTBar_ == 0 || handTBar_ == kTBarEnd) {
        // 0xb7054 at the take's end. Only the end where its divu overflows (Shrink / WIPE IN at 511,
        // Expand / WIPE OUT at 0) gives a path from the centre; at the other the size would stay and
        // the picture slide off in later takes, so the position is dropped there instead.
        if (handTBar_ != (r->flags ? 0 : kTBarEnd)) position_ = {};
        else if (position_.h || position_.v) position_.bakedAt = handTBar_;
    } else if (tbar_ == handTBar_ && !auto_) {
        positioning_ = true;
    }
}

void Switcher::movePosition(int dx, int dy) {
    if (!positioning_) return;
    const PositionRange r = *effect_->positionRange();
    position_.v = std::clamp(position_.v + algoMouseStep(dy, r.accel), -r.halfV, r.halfV - 1);
    position_.h = std::clamp(position_.h + algoMouseStep(dx, r.accel), -r.halfH, r.halfH - 1);
}

// 0xb71dc's exits. The T-bar keeps to 1..510 so the take stays open (stop >> 7 can be 0 or 511
// for an offset near the edge of the range, POSITION.md 10.3).
void Switcher::endPositioning(PositionEnd how) {
    if (!std::exchange(positioning_, false)) return;
    switch (how) {
        case PositionEnd::KeepAndStop:
            position_.bakedAt = tbar_;
            position_.stop = algoBakeStop(*effect_->positionRange(), position_.h, position_.v, tbar_);
            tbar_ = std::clamp(int(position_.stop >> 7), 1, kTBarEnd - 1);  // $1ec = stop >> 7
            break;
        case PositionEnd::Finish:  // 0xb78b4 for all four (POSITION.md 10.1: not 0xb78ce)
            position_.bakedAt = tbar_;
            tbar_ = kTBarEnd;
            finishAt511();
            break;
        case PositionEnd::BackToTBar: dragging_ = true; break;
    }
}

void Switcher::autoTransition(Speed speed, Direction dir) {
    if (effect_->isArtCardKey()) return;  // the card is held: Auto does nothing
    positioning_ = false;
    speed_ = speed;
    const Timing t = effect_->timing();
    if (dir == Direction::Forward) {
        if (tbar_ >= kTBarEnd) return;  // only a ChromaFX or an overlay can rest at 511
        beginTransitionIfIdle();
        if (staysEngaged()) engage();
    } else if (tbar_ == 0 && !auto_) {
        return;  // nothing to run back
    }
    // Only a forward Auto from T-bar 0 has Auto steps; a reverse one, or one turned round,
    // follows the T-bar. (Read only while auto_, so an Auto that ends needs no clearing: the
    // next one sets it afresh.)
    autoFrom0_ = dir == Direction::Forward && (auto_ ? autoFrom0_ : tbar_ == 0);
    const auto s = static_cast<size_t>(t.resolve(speed));
    // (Over 2^24 ticks, 78 hours, only corrupt tags; the cap keeps the fixed point in range.)
    const int dur = std::clamp(t.durationTicks[s], 1, 1 << 24);
    // Exact fixed point: the T-bar is autoAcc_ / autoDur_ rounded, and every tick adds (or, in
    // reverse, takes away) 511, so the full 0..511 trip takes exactly autoDur_ ticks and the
    // last one lands on 511 (0). The elapsed tick count from T-bar 0 is autoAcc_ / 511. A
    // running Auto keeps its exact position, rescaled to the new rate.
    autoAcc_ = auto_ ? autoAcc_ * dur / autoDur_ : static_cast<int64_t>(tbar_) * dur;
    autoDur_ = dur;
    autoFpf_ = std::max(0, t.fieldsPerFrame[s]);
    autoDir_ = dir;
    auto_ = true;
}

void Switcher::take() {
    const bool overlay = overlayEngaged();
    const bool card = effect_->isArtCardKey();
    auto_ = false;
    positioning_ = false;
    tbar_ = 0;
    disengage();
    soundPending_ = false;
    grabSelects_ = false;  // a pending grab must not undo the cut
    if (overlay) {  // Ruling 27: an overlay only ends; it cuts nothing
        sel_[busIndex(Bus::Superimpose)] = Source::None;  // C7: the key dropped at take start
        return;
    }
    // KEY.md 4 / design 2 B: swap; if Preview was Main, the key source goes to Preview; Superimpose
    // is emptied (the keyer mode stays).
    Source& main = sel_[busIndex(Bus::Main)];
    Source& prev = sel_[busIndex(Bus::Preview)];
    Source& sup = sel_[busIndex(Bus::Superimpose)];
    const bool same = main == prev;
    std::swap(main, prev);
    if (same && sup != Source::None) prev = sup;
    if (card && sup != Source::None) cardSaved_ = sup;
    sup = Source::None;
    if (card) applyCard();  // KEY.md 6 step 3: the cut keeps the card
}

void Switcher::tick() {
    ++tickCount_;
    if (engaged_) ++engagedTicks_;
    // The grab's wait is over: DV1 goes on Main and the store freezes (FREEZE.md 3: grab steps
    // 7-8, then message 0xb to the MAIN DV1 node). The next render writes the frame.
    if (grabPending_ && grabWait_ > 0 && --grabWait_ == 0 && grabSelects_) {
        sel_[busIndex(Bus::Main)] = Source::DV1;
        frozen_ = true;
    }
    if (!auto_) return;
    // ALGO.md 15.3: a forward Auto from 0 that has rendered a Positionable move's stop (t clamped
    // to it) holds there, the take open.
    if (position_.stop && autoFrom0_ && autoDir_ == Direction::Forward) {
        const auto r = effect_->positionRange();
        const int k = static_cast<int>(std::min<int64_t>(autoDur_, autoAcc_ / kTBarEnd + 1));  // the step rendered
        if (r && algoAutoT(k, autoDur_, r->flags) >= position_.stop) {
            auto_ = false;
            tbar_ = std::clamp(int(position_.stop >> 7), 1, kTBarEnd - 1);
            return;
        }
    }
    if (autoDir_ == Direction::Forward) {
        autoAcc_ += kTBarEnd;
        if (autoAcc_ >= static_cast<int64_t>(kTBarEnd) * autoDur_) {
            tbar_ = kTBarEnd;
            finishAt511();
        } else {
            // 511 means finished: a long Auto (over 1022 ticks) must not round up to it early.
            tbar_ = std::min(kTBarEnd - 1, static_cast<int>((autoAcc_ + autoDur_ / 2) / autoDur_));
        }
    } else {
        autoAcc_ -= kTBarEnd;
        if (autoAcc_ <= 0) arriveAt0();
        else tbar_ = std::max(1, static_cast<int>((autoAcc_ + autoDur_ / 2) / autoDur_));
    }
}

// The picture of one source. BKG is the colour, or noise that changes every tick, built only
// when first asked for (bkgBuilt); a missing input is black (blackBuilt). The frames are members.
const Frame& Switcher::sourceFrame(Source s, const SourceFrames& in, bool& bkgBuilt, bool& blackBuilt) const {
    const Frame* f = nullptr;
    switch (s) {
        case Source::In1: case Source::In2: case Source::In3: case Source::In4:
            f = in.inputs[static_cast<size_t>(s)];  // null = no picture, black
            break;
        case Source::DV1: case Source::DV2:
            f = frozen_ ? &stills_[size_t(dvIndex(s))] : delayed(dvInput_);  // live: one DV input
            break;
        case Source::Bkg:
            if (!bkgBuilt) {
                if (bkg_.isSnow()) {
                    renderSnow(bkgScratch_, tickCount_);
                } else {
                    std::fill(bkgScratch_.px.begin(), bkgScratch_.px.end(), bkg_.rgb());
                }
                bkgBuilt = true;
            }
            f = &bkgScratch_;
            break;
        case Source::None:  // an empty Superimpose bus; never asked for in practice
            break;
    }
    if (f) return *f;
    if (!blackBuilt) {
        std::fill(blackScratch_.px.begin(), blackScratch_.px.end(), kBlack);
        blackBuilt = true;
    }
    return blackScratch_;
}

void Switcher::renderSource(Source s, const SourceFrames& in, Frame& out) const {
    bool bkgBuilt = false, blackBuilt = false;
    const Frame& f = sourceFrame(s, in, bkgBuilt, blackBuilt);
    if (&f != &out) out.px = f.px;  // a copy into out's own buffer (out may be an input frame)
}

void Switcher::render(const SourceFrames& in, Frame& program) const {
    if (grabPending_ && grabWait_ == 0) {
        writeStill(0, in.inputs[size_t(grabInput_)]);
        grabPending_ = false;
    }
    record(dvInput_, in);
    // A digital move's buses go through DV too (RenderCtx::digitalA / digitalB), so their inputs
    // need a history from before the take starts.
    const bool digital = effect_->digitalMove() != DigitalMove::None;
    if (digital) {
        const Source m = sel_[busIndex(Bus::Main)], p = sel_[busIndex(Bus::Preview)];
        const Source s = sel_[busIndex(Bus::Superimpose)];
        if (isInput(m) && int(m) != dvInput_) record(int(m), in);
        if (isInput(p) && p != m && int(p) != dvInput_) record(int(p), in);
        if (isInput(s) && s != m && s != p && int(s) != dvInput_) record(int(s), in);
    }
    // The DV path's picture of a bus: its input, delayed, while the store is live.
    auto digitalOf = [&](Source s) { return digital && !frozen_ && isInput(s) ? delayed(int(s)) : nullptr; };
    bool bkgBuilt = false, blackBuilt = false;
    auto pick = [&](Source s) -> const Frame& { return sourceFrame(s, in, bkgBuilt, blackBuilt); };

    const Frame& a = pick(sel_[busIndex(Bus::Main)]);
    // Output goes to a separate frame: DVE effects must not alias their inputs, and `program`
    // may be one of the input frames. The scratch frames are members and `program`'s buffer is
    // swapped in and out, so a steady stream of renders allocates nothing.
    Frame& out = outScratch_;
    out.px.resize(size_t(kRasterW) * kRasterH);  // program's buffer may have come back empty
    if (effect_->isArtCardKey()) {
        // Design 2 C: the card on Superimpose cuts the hole through which Main shows over Preview.
        const Source sup = sel_[busIndex(Bus::Superimpose)];
        if (keyOn_ && sup != Source::None) {
            const int l = std::min(keyLevel_, kCardLevelMax);
            lumaKey(a, pick(sel_[busIndex(Bus::Preview)]), pick(sup), keyBlack_ ? l : kCardLevelMax - l, keyBlack_,
                    guesses_.keySoftness, out);
        } else {
            out.px = a.px;
        }
    } else if (!transitionActive()) {
        out.px = a.px;
        // The key: a Superimpose source that is not Main, over Main (design 2 A).
        const Source sup = sel_[busIndex(Bus::Superimpose)];
        if (keyOn_ && sup != Source::None && sup != sel_[busIndex(Bus::Main)] && keyLevel_ < kKeyLevelMax) {
            const Frame& s = pick(sup);
            if (keyLevel_ == 0) out.px = s.px;  // L 0: the Superimpose source alone
            else lumaKey(s, a, s, keyClip(keyBlack_, keyLevel_), keyBlack_, guesses_.keySoftness, out);
        }
    } else {
        const bool overlay = overlayEngaged();
        const EffectColour colour = effectColour();
        // The effect colour as a picture: one colour, or this tick's Snow.
        auto colourFrame = [&]() -> const Frame& {
            if (colour.isSnow()) {
                renderSnow(colourScratch_, tickCount_);
                colourValid_ = false;
            } else if (!colourValid_ || !(colourBuilt_ == colour)) {
                std::fill(colourScratch_.px.begin(), colourScratch_.px.end(), colour.rgb());
                colourBuilt_ = colour;
                colourValid_ = true;
            }
            return colourScratch_;
        };
        // A Snow generator's noise, for the matte areas of animated wipes too (RenderCtx::generator).
        const Frame* snow = colour.isSnow() ? &colourFrame() : nullptr;
        const Frame* bp = nullptr;
        const Frame* digitalB = nullptr;
        switch (overlay ? overlayB(*effect_) : OverlayB::Preview) {
            case OverlayB::Preview:
                bp = &pick(sel_[busIndex(Bus::Preview)]);
                digitalB = digitalOf(sel_[busIndex(Bus::Preview)]);
                break;
            case OverlayB::Main:
                bp = &a;
                digitalB = digitalOf(sel_[busIndex(Bus::Main)]);
                break;
            case OverlayB::Colour: bp = snow ? snow : &colourFrame(); break;
        }
        const Frame& b = *bp;
        const Timing timing = effect_->timing();
        const Speed speed = timing.resolve(speed_);
        RenderCtx ctx;
        ctx.background = colour.rgb();  // the effect's own colour if it has one (Ruling 26), else BKG's
        ctx.generator = snow;
        // The frame counter runs while a ChromaFX (cycling) or an overlay is engaged. Other
        // effects are rendered from the T-bar position alone, so a T-bar moved back looks like a
        // fresh render there.
        ctx.frameNumber = engaged_ ? engagedTicks_ : 0;
        ctx.speed = speed;
        ctx.guesses = guesses_;
        ctx.take = take_;
        ctx.position = position_;
        ctx.colour = colour;
        ctx.digitalA = digitalOf(sel_[busIndex(Bus::Main)]);
        ctx.digitalB = digitalB;
        // A forward Auto from 0 renders elapsed ticks 0 .. total-1 as Auto steps 1 .. total.
        if (auto_ && autoFrom0_ && autoDir_ == Direction::Forward) {
            ctx.autoStep = static_cast<int>(std::min<int64_t>(autoDur_, autoAcc_ / kTBarEnd + 1));
            ctx.autoSteps = autoDur_;  // Ruling 7: the Auto's own rate, whatever setSpeed did since
        }
        // Auto: the exact animation frame for the elapsed ticks (Ruling 25). Forward shows
        // elapsed ticks 0 .. total-1, reverse total-1 .. 0, each frame for autoFpf_ ticks.
        if (auto_ && autoFpf_ > 0) {
            const int64_t elapsed = autoDir_ == Direction::Forward ? autoAcc_ / kTBarEnd
                                                                   : std::max<int64_t>(0, autoAcc_ - 1) / kTBarEnd;
            ctx.animFrame = static_cast<int>(std::min<int64_t>(elapsed / autoFpf_, INT_MAX));
        }
        // An engaged animated overlay loops its animation whatever the T-bar or Auto does
        // (Ruling 27); a gradient one (fpf 0) follows the T-bar.
        const int fpf = timing.fieldsPerFrame[static_cast<size_t>(speed)];
        if (overlay && fpf > 0 && effect_->frameCount() > 0)
            ctx.animFrame = static_cast<int>(engagedTicks_ / uint32_t(fpf) % uint32_t(effect_->frameCount()));
        const KeyMove fly = flight();
        if (fly == KeyMove::Drops) {
            effect_->render(tbar_, a, b, out, ctx);
        } else {
            // Design 2 B: the move flies a picture over the polarity's colour (C3, C9), and that is
            // keyed by its own luma as the idle key is. Two-phase: from T-bar 256 Main's picture
            // over the Superimpose source (C4).
            const Source main = sel_[busIndex(Bus::Main)], sup = sel_[busIndex(Bus::Superimpose)];
            const bool second = fly == KeyMove::TwoPhase && tbar_ >= 256;
            const Source sa = fly == KeyMove::FliesOn ? sel_[busIndex(Bus::Preview)] : sup;
            const Source sb = fly == KeyMove::TwoPhase ? main : sa;
            const Source keyed = second ? main : sa;
            const Source under = second ? sup : main;
            ctx.keyFill = bkgColour(keyBlack_ ? BkgColour::Black : BkgColour::White);
            ctx.digitalA = digitalOf(sa);
            ctx.digitalB = digitalOf(sb);
            // Rendered even when nothing shows of it: an ALGO move steps its take's VM per render.
            Frame& f = keyScratch_;
            effect_->render(tbar_, pick(sa), pick(sb), f, ctx);
            const Frame& u = pick(under);
            if (keyed == under || keyLevel_ == kKeyLevelMax) out.px = u.px;  // no key condition, or L 257
            else if (keyLevel_ == 0) out.px = f.px;
            else lumaKey(f, u, f, keyClip(keyBlack_, keyLevel_), keyBlack_, guesses_.keySoftness, out);
        }
    }
    program.px.swap(out.px);
}

// --- DV store ---

void Switcher::record(int input, const SourceFrames& in) const {
    auto& frames = hist_[size_t(input)];
    auto& ticks = histTicks_[size_t(input)];
    const size_t n = size_t(digitalDelay(guesses_)) + 1;
    if (frames.size() != n) {  // first use, or the delay changed: start afresh
        frames.resize(n);
        ticks.assign(n, -1);
    }
    const size_t slot = tickCount_ % n;
    if (const Frame* f = in.inputs[size_t(input)]) frames[slot].px = f->px;
    else std::fill(frames[slot].px.begin(), frames[slot].px.end(), kBlack);
    ticks[slot] = tickCount_;
}

const Frame* Switcher::delayed(int input) const {
    const auto& ticks = histTicks_[size_t(input)];
    const int64_t now = int64_t(tickCount_);
    const int64_t want = now - digitalDelay(guesses_);
    int newest = -1, oldest = -1;
    for (int i = 0; i < int(ticks.size()); ++i) {
        const int64_t t = ticks[size_t(i)];
        // Ruling 4: a frame older than the ring's span is stale (left from an earlier spell as
        // the DV input), so "the oldest" is the oldest of the current unbroken run. Checked
        // here rather than in record so a freeze before the switch-back's first render is covered.
        if (t < 0 || t + int64_t(ticks.size()) < now) continue;
        if (t <= want && (newest < 0 || t > ticks[size_t(newest)])) newest = i;
        if (oldest < 0 || t < ticks[size_t(oldest)]) oldest = i;
    }
    const int i = newest >= 0 ? newest : oldest;
    return i >= 0 ? &hist_[size_t(input)][size_t(i)] : nullptr;
}

void Switcher::writeStill(int dv, const Frame* f) const {
    Frame& still = stills_[size_t(dv)];
    if (f) still.px = f->px;
    else std::fill(still.px.begin(), still.px.end(), kBlack);
    written_[size_t(dv)] = true;
}

const Frame& Switcher::still(Source dv) const { return stills_[size_t(dvIndex(dv))]; }

// FREEZE, LIVE and grab wait for the effect (design B: the DV memory is shared, spec 5.8); an
// engaged overlay does not hold them up.
bool Switcher::setFrozen(bool frozen, std::string* whyNot) {
    if (transitionActive() && !overlayEngaged()) {
        if (whyNot) *whyNot = kFinishEffect;
        return false;
    }
    if (frozen && !frozen_) {
        // The live picture goes into one still, DV2 if a bus shows DV2, else DV1; the other is kept.
        const bool dv2 = std::find(sel_.begin(), sel_.end(), Source::DV2) != sel_.end();
        writeStill(dv2 ? 1 : 0, delayed(dvInput_));
    }
    frozen_ = frozen;
    return true;
}

// FREEZE.md 3 (0xc3774): freeze if live, wait 4 fields, write the input into DV1; tick and
// render finish it.
bool Switcher::grab(int input, std::string* whyNot) {
    if (!setFrozen(true, whyNot)) return false;
    grabInput_ = std::clamp(input, 0, 3);
    grabWait_ = digitalDelay(guesses_);
    grabPending_ = true;
    grabSelects_ = true;
    if (grabWait_ == 0) sel_[busIndex(Bus::Main)] = Source::DV1;  // no wait for tick to end
    return true;
}

// FREEZE.md 4 (FRAM handler 0x122364).
bool Switcher::loadStill(const Frame& still, std::string* whyNot) {
    if ((auto_ || tbar_ > 0) && !engaged_) {
        if (whyNot) *whyNot = "a transition is running: finish it first";
        return false;
    }
    // Step 1: ChromaFX off, live digital stripped from the buses.
    if (chromaEngaged()) {
        auto_ = false;
        tbar_ = 0;
        disengage();
    }
    if (!frozen_)
        for (Source& s : sel_)
            if (isDv(s)) s = static_cast<Source>(dvInput_);
    // Step 2: the target DV.
    auto shows = [&](Bus b, Source s) { return sel_[busIndex(b)] == s; };
    Source target = Source::DV2;
    if (shows(Bus::Main, Source::DV1) || shows(Bus::Superimpose, Source::DV1)) target = Source::DV2;
    else if (shows(Bus::Main, Source::DV2) || shows(Bus::Superimpose, Source::DV2)) target = Source::DV1;
    else if (shows(Bus::Preview, Source::DV2)) target = Source::DV1;
    // Steps 4 and 6: the still frozen in the target, shown on Preview. A pending grab is
    // cancelled (Ruling 5): it would overwrite DV1 and force it onto Main.
    grabPending_ = false;
    writeStill(dvIndex(target), &still);
    frozen_ = true;
    sel_[busIndex(Bus::Preview)] = target;
    return true;
}

std::optional<Source> Switcher::saveSource(std::string* whyNot) const {
    if (!written_[0] && !written_[1]) {
        if (whyNot) *whyNot = "SAVE: no still in DV1/DV2";
        return std::nullopt;
    }
    for (Bus b : {Bus::Preview, Bus::Main})
        if (isDv(sel_[busIndex(b)])) return sel_[busIndex(b)];
    return Source::DV1;
}

std::shared_ptr<const Sound> Switcher::pendingSound() {
    if (!soundPending_) return nullptr;
    soundPending_ = false;
    const Sound* s = effect_->sound();
    if (!s) return nullptr;
    return std::shared_ptr<const Sound>(effect_, s);  // aliasing: owns the effect, points at its sound
}

}  // namespace vt
