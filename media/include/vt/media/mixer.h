#pragma once
// Audio follows video: the mixer takes the four inputs' audio for one tick and the Switcher's
// state after render(), and mixes what Program shows (spec 5.11). Per input the fader and mute
// come first, then the source gain that follows the Main/Preview buses and the T-bar, then the
// sum with the effect sounds, then the Master fader. There is no limiter; samples are clamped
// to +-1. Every gain change, a cut included, ramps linearly across the tick's samples, so a cut
// is a one-tick (16.7 ms) crossfade rather than a click.
#include <array>
#include <atomic>
#include <cmath>
#include <limits>
#include <memory>
#include <mutex>
#include <span>
#include <vector>

#include "vt/media/audio_convert.h"
#include "vt/sound.h"
#include "vt/switcher.h"

namespace vt::media {

enum class Channel { In1, In2, In3, In4, EffectSound, Master };

// What Program shows, from the Switcher after render().
struct MixState {
    vt::Source main = vt::Source::In1, preview = vt::Source::In1;
    int tbar = 0;
    bool transition = false, chroma = false, overlay = false;
    static MixState of(const vt::Switcher&);
};

// Post-fader level of one channel since the previous meters() read (-inf dB for silence).
struct Meter {
    float peakDb = -std::numeric_limits<float>::infinity();
    float rmsDb = -std::numeric_limits<float>::infinity();
};

// Source gains: with a transition running (and neither ChromaFX nor an overlay engaged) Main is
// cos(p*pi/2) and Preview sin(p*pi/2), p = tbar/511; with none Main is 1 and Preview 0. An
// engaged ChromaFX shows the recoloured Preview, so the audio is Preview 1, Main 0; an engaged
// overlay keeps Main 1, Preview 0. BKG, DV1 and DV2 carry no audio, and a source on both buses
// has gain 1 (not cos + sin).
//
// Meters are taken after the channel's fader and mute (so an input that is not on air still
// reads its level); Master's is the final output, after its fader and the clamp. Each meters()
// call returns, per channel, the peak since the previous call and the RMS over every sample
// processed since then, and starts the next interval; when no tick ran in between it repeats
// its previous reading, so a display polling faster than 59.94 Hz does not flicker.
//
// Threading: process() runs on one thread. setFaderDb, setMute and meters() may be called from
// any thread at any time; a change takes effect from the next process() call. meters() and
// process() share a lock held only to add or take the level sums (no allocation, no slow call).
class AudioMixer {
public:
    AudioMixer();

    // Default 0 dB. Clamped to [-96, +24] dB, where -96 (or less) is silence; NaN is ignored
    // (the fader keeps its value).
    void setFaderDb(Channel, float db);
    void setMute(Channel, bool);

    // Mixes one tick. inputs[i] holds that tick's stereo samples of input i (silence if none or
    // short); `out` is the tick's stereo samples (800 or 801 frames). A non-null `sound` starts
    // that effect sound at this tick's first sample; it plays to its end whatever the Switcher
    // does next, overlapping sounds sum. No allocation, except on the tick a sound starts for
    // the first time (its converted audio is cached while the Sound is alive).
    void process(const MixState&, const std::array<std::span<const float>, 4>& inputs,
                 std::shared_ptr<const vt::Sound> sound, std::span<float> out);

    std::array<Meter, 6> meters(); // since the previous call, indexed by Channel; any thread

private:
    static constexpr int kChannels = 6;
    static constexpr size_t kVoices = 16;
    struct Voice {
        std::shared_ptr<const std::vector<float>> pcm; // 48 kHz stereo; null = free
        size_t pos = 0;                                // next frame
    };
    struct Cached {
        std::weak_ptr<const vt::Sound> sound;
        std::shared_ptr<const std::vector<float>> pcm;
    };
    void startSound(const std::shared_ptr<const vt::Sound>&);

    // Written by any thread.
    std::array<std::atomic<float>, kChannels> fader_; // linear
    std::array<std::atomic<bool>, kChannels> mute_;

    // Levels since the last meters() read: process() adds, meters() takes.
    struct Level {
        float peak = 0;          // linear
        double sumSquares = 0;   // both stereo channels
        int64_t frames = 0;      // stereo frames summed
    };
    std::mutex meterMu_;
    std::array<Level, kChannels> levels_;
    std::array<Meter, kChannels> lastMeters_; // the previous reading

    // process() thread only.
    std::array<float, kChannels> lastStrip_;  // fader * mute of the last tick
    std::array<float, 4> lastFollow_{};       // source gains of the last tick
    std::array<Voice, kVoices> voices_;
    std::vector<Cached> cache_;
    AudioConverter converter_;
};

} // namespace vt::media
