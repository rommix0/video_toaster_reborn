#include "vt/media/mixer.h"

#include <algorithm>
#include <numbers>

namespace vt::media {

namespace {

constexpr float kSilenceDb = -96.0f, kMaxDb = 24.0f;

float dbToLinear(float db) { return db > kSilenceDb ? std::pow(10.0f, std::min(db, kMaxDb) / 20.0f) : 0.0f; }

float toDb(double linear) {
    return linear > 0 ? static_cast<float>(20.0 * std::log10(linear)) : -std::numeric_limits<float>::infinity();
}

// Input index of a source that carries audio, or -1.
int inputOf(vt::Source s) { return s <= vt::Source::In4 ? static_cast<int>(s) : -1; }

// Level of one channel over a tick, both stereo channels together.
struct TickLevel {
    float peak = 0;
    double sumSquares = 0;
    void add(float l, float r) {
        peak = std::max({peak, std::fabs(l), std::fabs(r)});
        sumSquares += double(l) * l + double(r) * r;
    }
};

} // namespace

// Art Card Key holds its T-bar (the clip control) with no transition running: audio follows Main.
MixState MixState::of(const vt::Switcher& sw) {
    return {sw.selected(vt::Bus::Main), sw.selected(vt::Bus::Preview), sw.tbar(),
            sw.transitionActive() && !sw.effect().isArtCardKey(), sw.chromaEngaged(), sw.overlayEngaged()};
}

AudioMixer::AudioMixer() {
    for (int c = 0; c < kChannels; ++c) {
        fader_[c] = 1.0f;
        mute_[c] = false;
    }
    lastStrip_.fill(1.0f);
}

void AudioMixer::setFaderDb(Channel c, float db) {
    if (std::isnan(db)) return;
    fader_[static_cast<int>(c)] = dbToLinear(db);
}

void AudioMixer::setMute(Channel c, bool m) { mute_[static_cast<int>(c)] = m; }

std::array<Meter, 6> AudioMixer::meters() {
    std::lock_guard l(meterMu_);
    for (int c = 0; c < kChannels; ++c) {
        Level& v = levels_[c];
        if (v.frames == 0) continue; // no tick since the last read: repeat it
        lastMeters_[c] = {toDb(v.peak), toDb(std::sqrt(v.sumSquares / double(2 * v.frames)))};
        v = {};
    }
    return lastMeters_;
}

void AudioMixer::startSound(const std::shared_ptr<const vt::Sound>& sound) {
    std::shared_ptr<const std::vector<float>> pcm;
    for (const Cached& c : cache_)
        if (c.sound.lock() == sound) { pcm = c.pcm; break; }
    if (!pcm) {
        std::span<const float> converted = converter_.convert(*sound);
        if (converted.size() < 2) return; // empty or undecodable: nothing to play
        pcm = std::make_shared<const std::vector<float>>(converted.begin(), converted.end());
        std::erase_if(cache_, [](const Cached& c) { return c.sound.expired(); });
        cache_.push_back({sound, pcm});
    }
    // A free voice, else the one nearest its end.
    Voice* v = &voices_[0];
    for (Voice& c : voices_) {
        if (!c.pcm) { v = &c; break; }
        if (c.pcm->size() - c.pos < v->pcm->size() - v->pos) v = &c;
    }
    *v = {std::move(pcm), 0};
}

void AudioMixer::process(const MixState& st, const std::array<std::span<const float>, 4>& inputs,
                         std::shared_ptr<const vt::Sound> sound, std::span<float> out) {
    if (sound) startSound(sound);
    const size_t n = out.size() / 2;
    if (n == 0) return;

    // Source gains for this tick.
    std::array<float, 4> follow{};
    float mainGain = 1, previewGain = 0;
    if (st.chroma) {
        mainGain = 0, previewGain = 1;
    } else if (!st.overlay && st.transition) {
        const double p = std::clamp(st.tbar, 0, 511) / 511.0;
        mainGain = static_cast<float>(std::cos(p * std::numbers::pi / 2));
        previewGain = static_cast<float>(std::sin(p * std::numbers::pi / 2));
    }
    const int mi = inputOf(st.main), pi = inputOf(st.preview);
    if (mi >= 0 && mi == pi) {
        follow[mi] = 1;
    } else {
        if (mi >= 0) follow[mi] = mainGain;
        if (pi >= 0) follow[pi] = previewGain;
    }

    std::array<float, kChannels> strip;
    for (int c = 0; c < kChannels; ++c) strip[c] = mute_[c].load() ? 0.0f : fader_[c].load();

    std::array<size_t, 4> avail;
    for (int i = 0; i < 4; ++i) avail[i] = std::min(inputs[i].size() / 2, n);

    std::array<TickLevel, kChannels> level;
    const float invN = 1.0f / static_cast<float>(n);
    const int fx = static_cast<int>(Channel::EffectSound), master = static_cast<int>(Channel::Master);
    for (size_t j = 0; j < n; ++j) {
        const float t = static_cast<float>(j + 1) * invN; // reaches 1 on the tick's last sample
        float l = 0, r = 0;
        for (int i = 0; i < 4; ++i) {
            if (j >= avail[i]) continue;
            const float g = lastStrip_[i] + (strip[i] - lastStrip_[i]) * t;
            const float sl = inputs[i][2 * j] * g, sr = inputs[i][2 * j + 1] * g;
            level[i].add(sl, sr);
            const float f = lastFollow_[i] + (follow[i] - lastFollow_[i]) * t;
            l += sl * f;
            r += sr * f;
        }
        float el = 0, er = 0;
        for (const Voice& v : voices_) {
            if (v.pcm && v.pos + j < v.pcm->size() / 2) {
                el += (*v.pcm)[2 * (v.pos + j)];
                er += (*v.pcm)[2 * (v.pos + j) + 1];
            }
        }
        const float ge = lastStrip_[fx] + (strip[fx] - lastStrip_[fx]) * t;
        el *= ge, er *= ge;
        level[fx].add(el, er);
        const float gm = lastStrip_[master] + (strip[master] - lastStrip_[master]) * t;
        l = std::clamp((l + el) * gm, -1.0f, 1.0f);
        r = std::clamp((r + er) * gm, -1.0f, 1.0f);
        level[master].add(l, r);
        out[2 * j] = l;
        out[2 * j + 1] = r;
    }
    if (out.size() & 1) out[out.size() - 1] = 0;

    lastStrip_ = strip;
    lastFollow_ = follow;
    for (Voice& v : voices_) {
        if (!v.pcm) continue;
        v.pos += n;
        if (v.pos >= v.pcm->size() / 2) v = {};
    }
    std::lock_guard l(meterMu_);
    for (int c = 0; c < kChannels; ++c) {
        Level& v = levels_[c];
        v.peak = std::max(v.peak, level[c].peak);
        v.sumSquares += level[c].sumSquares;
        v.frames += int64_t(n);
    }
}

} // namespace vt::media
