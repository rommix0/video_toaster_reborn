#include "vt/switcher.h"

#include <algorithm>
#include <climits>
#include <utility>

namespace vt {
namespace {

constexpr int kTBarEnd = 511;
constexpr Rgba8 kBlack{0, 0, 0, 255};

size_t busIndex(Bus b) { return static_cast<size_t>(b); }

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
    std::fill(dv_.px.begin(), dv_.px.end(), kBlack);
}

void Switcher::select(Bus b, Source s) { sel_[busIndex(b)] = s; }
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
    effectColour_ = effect_->effectColour();
    speed_ = effect_->timing().defaultSpeed;
    return true;
}

void Switcher::beginTransitionIfIdle() {
    if (!transitionActive()) soundPending_ = effect_->sound() != nullptr;
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

void Switcher::render(const SourceFrames& in, Frame& program) const {
    // BKG: the colour, or noise that changes every tick. Built only if a bus uses it.
    const bool snow = bkg_.isSnow();
    bool bkgBuilt = false;

    auto frameOf = [&](Source s) -> const Frame* {
        switch (s) {
            case Source::In1: case Source::In2: case Source::In3: case Source::In4:
                return in.inputs[static_cast<size_t>(s)];  // null = no picture, black
            case Source::DV1: case Source::DV2:
                return &dv_;
            case Source::Bkg:
                if (!bkgBuilt) {
                    if (snow) {
                        renderSnow(bkgScratch_, tickCount_);
                    } else {
                        std::fill(bkgScratch_.px.begin(), bkgScratch_.px.end(), bkg_.rgb());
                    }
                    bkgBuilt = true;
                }
                return &bkgScratch_;
        }
        return nullptr;
    };
    bool blackBuilt = false;
    auto pick = [&](Source s) -> const Frame& {
        const Frame* f = frameOf(s);
        if (f) return *f;
        if (!blackBuilt) {
            std::fill(blackScratch_.px.begin(), blackScratch_.px.end(), kBlack);
            blackBuilt = true;
        }
        return blackScratch_;
    };

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
        const Frame* bp = nullptr;
        switch (overlay ? overlayB(*effect_) : OverlayB::Preview) {
            case OverlayB::Preview: bp = &pick(sel_[busIndex(Bus::Preview)]); break;
            case OverlayB::Main: bp = &a; break;
            case OverlayB::Colour:
                if (effectColour_.isSnow()) {
                    renderSnow(colourScratch_, tickCount_);
                    colourValid_ = false;
                } else if (!colourValid_ || !(colourBuilt_ == effectColour_)) {
                    std::fill(colourScratch_.px.begin(), colourScratch_.px.end(), effectColour_.rgb());
                    colourBuilt_ = effectColour_;
                    colourValid_ = true;
                }
                bp = &colourScratch_;
                break;
        }
        const Frame& b = *bp;
        const Timing timing = effect_->timing();
        const Speed speed = timing.resolve(speed_);
        RenderCtx ctx;
        ctx.background = effectColour_.rgb();  // the effect's own colour, not BKG's (Ruling 26)
        // The frame counter runs while a ChromaFX (cycling) or an overlay is engaged. Other
        // effects are rendered from the T-bar position alone, so a T-bar moved back looks like a
        // fresh render there.
        ctx.frameNumber = engaged_ ? engagedTicks_ : 0;
        ctx.speed = speed;
        ctx.guesses = guesses_;
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

std::shared_ptr<const Sound> Switcher::pendingSound() {
    if (!soundPending_) return nullptr;
    soundPending_ = false;
    const Sound* s = effect_->sound();
    if (!s) return nullptr;
    return std::shared_ptr<const Sound>(effect_, s);  // aliasing: owns the effect, points at its sound
}

}  // namespace vt
