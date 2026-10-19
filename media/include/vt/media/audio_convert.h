#pragma once
// Audio into the mixer's format and the timestamped ring it is read from. Everything the media
// layer hands to the mixer is 48 kHz stereo float, interleaved (L, R, L, R ...).
#include <cstdint>
#include <mutex>
#include <span>
#include <vector>

#include "vt/media/ffmpeg.h"
#include "vt/sound.h"

namespace vt::media {

class AudioConverter {
public:
    AudioConverter() = default;
    ~AudioConverter();
    AudioConverter(const AudioConverter&) = delete;
    AudioConverter& operator=(const AudioConverter&) = delete;

    // One frame of a stream, any sample format, rate and channel layout. The resampler keeps its
    // internal delay from call to call (nothing is flushed) and is rebuilt when the source
    // format, rate or layout changes. Mono goes to both channels at unity gain; more than two
    // channels are downmixed with swresample's default matrix. A source swresample cannot
    // convert gives an empty result (and one warning until the source changes).
    // The span is valid until the next call.
    std::span<const float> convert(const AVFrame& src);

    // A complete clip (an 8SVX effect): converted on its own, with the resampler's delay flushed
    // so the result has the clip's whole duration. Does not disturb the stream state above.
    // The span is valid until the next call.
    std::span<const float> convert(const vt::Sound& sound);

private:
    SwrPtr swr_;                 // the stream resampler; null before the first frame or on failure
    int format_ = -1, rate_ = 0; // what swr_ was built for, with layout_
    AVChannelLayout layout_{};   // owned
    std::vector<float> out_;
};

// The last `capacity` samples of a stream, addressed by absolute 48 kHz session sample index
// (SessionTime / 5). One writer thread and one reader thread may use it at the same time.
class AudioRing {
public:
    explicit AudioRing(int64_t capacitySamples = 48000 * 4);

    // Stores interleaved stereo at `firstSample`; earlier data at those positions is replaced.
    // A write past the end leaves silence between the old end and itself. Parts older than
    // the capacity (or before sample 0) are dropped.
    void write(int64_t firstSample, std::span<const float> stereo);

    // Fills `stereo` from `firstSample` on: silence where nothing was written, where the data
    // has been overwritten by newer samples, or before sample 0.
    void read(int64_t firstSample, std::span<float> stereo) const;

    // One past the newest sample written so far (0 for an empty ring).
    int64_t endSample() const;

private:
    int64_t cap_;
    mutable std::mutex mu_; // guards end_ and buf_; held only for the copy of one span
    int64_t end_ = 0;
    std::vector<float> buf_; // cap_ stereo frames; sample p lives at slot p % cap_
};

} // namespace vt::media
