#include "vt/switcher.h"

#include <algorithm>
#include <climits>
#include <utility>

namespace vt {
namespace {

constexpr int kTBarEnd = 511;
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

void Switcher::select(Bus b, Source s) {
    Source& cur = sel_[busIndex(b)];
    if (!frozen_ && isDv(s) && isInput(cur)) dvInput_ = static_cast<int>(cur);  // FREEZE.md 3
    cur = s;
}
Source Switcher::selected(Bus b) const { return sel_[busIndex(b)]; }

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
    // Abandon whatever was in progress, without a swap.
    auto_ = false;
    tbar_ = 0;
    disengage();
    soundPending_ = false;
    effect_ = std::move(e);
    ++take_;  // the new effect starts a new take (ALGO.md 15.1: a crouton selected ends the take)
    effectColour_ = effect_->effectColour();
    speed_ = effect_->timing().defaultSpeed;
    return true;
}

void Switcher::beginTransitionIfIdle() {
    if (transitionActive()) return;
    soundPending_ = effect_->sound() != nullptr;
    ++take_;
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
    } else {
        std::swap(sel_[busIndex(Bus::Main)], sel_[busIndex(Bus::Preview)]);
        tbar_ = 0;  // the effect is finished; the T-bar counts as back at 0
    }
}

// The T-bar is back at 0 (by hand or by a reverse Auto): nothing swaps; an overlay ends; a
// Transition-mode ChromaFX disengages, a Filter-mode one stays engaged.
void Switcher::arriveAt0() {
    auto_ = false;
    tbar_ = 0;
    if (!effect_->isChromaFilter()) disengage();
}

void Switcher::setTBar(int pos) {
    pos = std::clamp(pos, 0, kTBarEnd);
    if (pos == tbar_) return;  // also keeps a running Auto going if a UI echoes the T-bar back
    auto_ = false;             // the hand takes over
    if (tbar_ == 0 && pos > 0) {
        beginTransitionIfIdle();
        if (staysEngaged()) engage();
    }
    tbar_ = pos;
    if (pos == 0) arriveAt0();
    else if (pos == kTBarEnd && !staysEngaged()) finishAt511();
}

void Switcher::autoTransition(Speed speed, Direction dir) {
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
    auto_ = false;
    tbar_ = 0;
    disengage();
    soundPending_ = false;
    if (!overlay) std::swap(sel_[busIndex(Bus::Main)], sel_[busIndex(Bus::Preview)]);  // Ruling 27
}

void Switcher::tick() {
    ++tickCount_;
    if (engaged_) ++engagedTicks_;
    // The grab's wait is over: DV1 goes on Main and the store freezes (FREEZE.md 3: grab steps
    // 7-8, then message 0xb to the MAIN DV1 node). The next render writes the frame.
    if (grabPending_ && grabWait_ > 0 && --grabWait_ == 0) {
        sel_[busIndex(Bus::Main)] = Source::DV1;
        frozen_ = true;
    }
    if (!auto_) return;
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
    bool bkgBuilt = false, blackBuilt = false;
    auto pick = [&](Source s) -> const Frame& { return sourceFrame(s, in, bkgBuilt, blackBuilt); };

    const Frame& a = pick(sel_[busIndex(Bus::Main)]);
    // Output goes to a separate frame: DVE effects must not alias their inputs, and `program`
    // may be one of the input frames. (The Overlay bus has no keying in version 1.) The scratch
    // frames are members and `program`'s buffer is swapped in and out, so a steady stream of
    // renders allocates nothing.
    Frame& out = outScratch_;
    out.px.resize(size_t(kRasterW) * kRasterH);  // program's buffer may have come back empty
    if (!transitionActive()) {
        out.px = a.px;
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
        switch (overlay ? overlayB(*effect_) : OverlayB::Preview) {
            case OverlayB::Preview: bp = &pick(sel_[busIndex(Bus::Preview)]); break;
            case OverlayB::Main: bp = &a; break;
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
        ctx.colour = colour;
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
        effect_->render(tbar_, a, b, out, ctx);
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
        if (t < 0 || t + int64_t(ticks.size()) <= now) continue;
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
    if (shows(Bus::Main, Source::DV1) || shows(Bus::Overlay, Source::DV1)) target = Source::DV2;
    else if (shows(Bus::Main, Source::DV2) || shows(Bus::Overlay, Source::DV2)) target = Source::DV1;
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
