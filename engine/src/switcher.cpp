#include "vt/switcher.h"

#include <algorithm>
#include <climits>
#include <utility>

namespace vt {
namespace {

constexpr int kTBarEnd = 511;
constexpr Rgba8 kBlack{0, 0, 0, 255};

size_t busIndex(Bus b) { return static_cast<size_t>(b); }

}  // namespace

Switcher::Switcher() : effect_(makeDissolve()), effectColour_(effect_->effectColour()) {
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
    const bool replacingChroma = chromaEngaged_ && e->isChromaFx();
    if (e->usesDvMemory() && !replacingChroma) {
        // ChromaFX and the DVE moves share the DV memory (spec 5.8): only one can use it at a time.
        if (chromaEngaged_) return refuse("a ChromaFX effect is engaged: press Take or return the T-bar to 0 first");
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
    return true;
}

void Switcher::beginTransitionIfIdle() {
    if (!transitionActive()) soundPending_ = effect_->sound() != nullptr;
}

void Switcher::disengage() {
    chromaEngaged_ = false;
    chromaFrames_ = 0;
}

void Switcher::finishAt511() {
    auto_ = false;
    if (effect_->isChromaFx()) {
        tbar_ = kTBarEnd;  // stays engaged, no swap (Ruling 21)
    } else {
        std::swap(sel_[busIndex(Bus::Main)], sel_[busIndex(Bus::Preview)]);
        tbar_ = 0;  // the effect is finished; the T-bar counts as back at 0
    }
}

void Switcher::setTBar(int pos) {
    pos = std::clamp(pos, 0, kTBarEnd);
    if (pos == tbar_) return;  // also keeps a running Auto going if a UI echoes the T-bar back
    auto_ = false;             // the hand takes over
    const bool chroma = effect_->isChromaFx();
    if (tbar_ == 0 && pos > 0) {
        beginTransitionIfIdle();
        if (chroma && !chromaEngaged_) {
            chromaEngaged_ = true;
            chromaFrames_ = 0;
        }
    }
    tbar_ = pos;
    if (chroma) {
        if (pos == 0 && !effect_->isChromaFilter()) disengage();
    } else if (pos == kTBarEnd) {
        finishAt511();
    }
}

void Switcher::autoTransition(Speed speed) {
    const Timing t = effect_->timing();
    const bool chroma = effect_->isChromaFx();
    if (tbar_ >= kTBarEnd) return;  // only a ChromaFX can rest at 511
    beginTransitionIfIdle();
    if (chroma && !chromaEngaged_) {
        chromaEngaged_ = true;
        chromaFrames_ = 0;
    }
    const auto s = static_cast<size_t>(t.resolve(speed));
    autoDur_ = std::max(1, t.durationTicks[s]);
    autoFpf_ = std::max(0, t.fieldsPerFrame[s]);
    // Exact fixed point: the T-bar is autoAcc_ / autoDur_ rounded, and every tick adds 511, so
    // the full 0..511 trip takes exactly autoDur_ ticks and the last one lands on 511. The
    // elapsed tick count (from T-bar 0) is autoAcc_ / 511.
    autoAcc_ = static_cast<int64_t>(tbar_) * autoDur_;
    auto_ = true;
}

void Switcher::take() {
    auto_ = false;
    tbar_ = 0;
    disengage();
    std::swap(sel_[busIndex(Bus::Main)], sel_[busIndex(Bus::Preview)]);
}

void Switcher::tick() {
    ++tickCount_;
    if (chromaEngaged_) ++chromaFrames_;
    if (!auto_) return;
    autoAcc_ += kTBarEnd;
    if (autoAcc_ >= static_cast<int64_t>(kTBarEnd) * autoDur_) {
        tbar_ = kTBarEnd;
        finishAt511();
    } else {
        // 511 means finished: a long Auto (over 1022 ticks) must not round up to it early.
        tbar_ = std::min(kTBarEnd - 1, static_cast<int>((autoAcc_ + autoDur_ / 2) / autoDur_));
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
        const Frame& b = pick(sel_[busIndex(Bus::Preview)]);
        RenderCtx ctx;
        ctx.background = effectColour_.rgb();  // the effect's own colour, not BKG's (Ruling 26)
        // Only ChromaFX uses the frame counter (cycling). Other effects are rendered from the
        // T-bar position alone, so a T-bar moved back looks like a fresh render there.
        ctx.frameNumber = chromaEngaged_ ? chromaFrames_ : 0;
        // Auto: the exact animation frame for the elapsed ticks (Ruling 25).
        if (auto_ && autoFpf_ > 0)
            ctx.animFrame = static_cast<int>(std::min<int64_t>(autoAcc_ / kTBarEnd / autoFpf_, INT_MAX));
        effect_->render(tbar_, a, b, out, ctx);
    }
    program.px.swap(out.px);
}

const Sound* Switcher::pendingSound() {
    if (!soundPending_) return nullptr;
    soundPending_ = false;
    return effect_->sound();
}

}  // namespace vt
