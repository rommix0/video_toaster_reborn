#include "vt/media/audio_convert.h"

#include <algorithm>
#include <cstring>
#include <string>

#include "vt/log.h"

namespace vt::media {
namespace {

constexpr int kRate = 48000;

// A resampler from (fmt, rate, layout) to 48 kHz stereo float; null (with a warning) on failure.
// A source that only gives a channel count gets the default layout for it.
SwrPtr makeResampler(AVSampleFormat fmt, int rate, const AVChannelLayout& layout) {
    AVChannelLayout in{};
    if (layout.nb_channels > 0 && layout.order != AV_CHANNEL_ORDER_UNSPEC)
        av_channel_layout_copy(&in, &layout);
    else
        av_channel_layout_default(&in, std::max(layout.nb_channels, 1));
    const AVChannelLayout out = AV_CHANNEL_LAYOUT_STEREO;
    SwrContext* raw = nullptr;
    int err = swr_alloc_set_opts2(&raw, &out, AV_SAMPLE_FMT_FLT, kRate, &in, fmt, rate, 0, nullptr);
    SwrPtr swr(raw);
    if (err >= 0 && in.nb_channels == 1) {
        // swresample's default would attenuate mono by 3 dB; both channels get it unchanged.
        const double matrix[2] = {1.0, 1.0};
        err = swr_set_matrix(swr.get(), matrix, 1);
    }
    if (err >= 0) err = swr_init(swr.get());
    av_channel_layout_uninit(&in);
    if (err < 0) {
        const char* name = av_get_sample_fmt_name(fmt);
        vt::logWarning("audio: cannot convert " + std::to_string(rate) + " Hz, " +
                       std::to_string(layout.nb_channels) + " channel audio (format " +
                       (name ? name : "?") + "): " + averr(err));
        return nullptr;
    }
    return swr;
}

// Appends the resampler's output for `inCount` input samples (or its buffered delay, for a null
// `in`) to `out`.
bool convertInto(SwrContext* swr, std::vector<float>& out, const uint8_t* const* in, int inCount) {
    const int cap = swr_get_out_samples(swr, inCount) + 64;
    if (cap <= 0) return false;
    const size_t at = out.size();
    out.resize(at + size_t(cap) * 2);
    uint8_t* dst = reinterpret_cast<uint8_t*>(out.data() + at);
    const int got = swr_convert(swr, &dst, cap, in, inCount);
    if (got < 0) {
        out.resize(at);
        return false;
    }
    out.resize(at + size_t(got) * 2);
    return true;
}

} // namespace

AudioConverter::~AudioConverter() { av_channel_layout_uninit(&layout_); }

std::span<const float> AudioConverter::convert(const AVFrame& src) {
    out_.clear();
    if (src.nb_samples <= 0 || src.sample_rate <= 0 || src.format < 0) return {};

    if (src.format != format_ || src.sample_rate != rate_ ||
        av_channel_layout_compare(&src.ch_layout, &layout_) != 0) {
        format_ = src.format;
        rate_ = src.sample_rate;
        av_channel_layout_uninit(&layout_);
        av_channel_layout_copy(&layout_, &src.ch_layout);
        swr_ = makeResampler(AVSampleFormat(src.format), src.sample_rate, src.ch_layout);
    }
    if (!swr_) return {};
    if (!convertInto(swr_.get(), out_, src.extended_data, src.nb_samples)) out_.clear();
    return out_;
}

std::span<const float> AudioConverter::convert(const vt::Sound& sound) {
    out_.clear();
    const int ch = sound.channels;
    if (sound.rate <= 0 || ch < 1 || sound.pcm.size() < size_t(ch)) return {};

    AVChannelLayout layout{};
    av_channel_layout_default(&layout, ch);
    SwrPtr swr = makeResampler(AV_SAMPLE_FMT_S16, sound.rate, layout);
    av_channel_layout_uninit(&layout);
    if (!swr) return {};

    const uint8_t* in = reinterpret_cast<const uint8_t*>(sound.pcm.data());
    const int frames = int(sound.pcm.size() / size_t(ch));
    if (!convertInto(swr.get(), out_, &in, frames) || !convertInto(swr.get(), out_, nullptr, 0)) out_.clear();
    return out_;
}

AudioRing::AudioRing(int64_t capacitySamples)
    : cap_(std::max<int64_t>(capacitySamples, 1)), buf_(size_t(cap_) * 2, 0.0f) {}

void AudioRing::write(int64_t firstSample, std::span<const float> stereo) {
    const int64_t n = int64_t(stereo.size() / 2);
    if (n == 0) return;
    std::lock_guard lock(mu_);
    const int64_t newEnd = std::max(end_, firstSample + n);
    const int64_t floor = std::max<int64_t>(newEnd - cap_, 0); // oldest sample that stays

    // Silence for the gap between the old end and this write (those slots hold older laps).
    for (int64_t p = std::max(end_, floor); p < firstSample;) {
        const int64_t slot = p % cap_, run = std::min(firstSample - p, cap_ - slot);
        std::fill_n(buf_.data() + slot * 2, run * 2, 0.0f);
        p += run;
    }
    // The part of the write that is inside the window.
    for (int64_t p = std::max(firstSample, floor); p < firstSample + n;) {
        const int64_t slot = p % cap_, run = std::min(firstSample + n - p, cap_ - slot);
        std::memcpy(buf_.data() + slot * 2, stereo.data() + (p - firstSample) * 2, size_t(run) * 2 * sizeof(float));
        p += run;
    }
    end_ = newEnd;
}

void AudioRing::read(int64_t firstSample, std::span<float> stereo) const {
    const int64_t n = int64_t(stereo.size() / 2);
    std::fill(stereo.begin(), stereo.end(), 0.0f);
    if (n == 0) return;
    std::lock_guard lock(mu_);
    const int64_t hi = std::min(firstSample + n, end_);
    for (int64_t p = std::max({firstSample, end_ - cap_, int64_t(0)}); p < hi;) {
        const int64_t slot = p % cap_, run = std::min(hi - p, cap_ - slot);
        std::memcpy(stereo.data() + (p - firstSample) * 2, buf_.data() + slot * 2, size_t(run) * 2 * sizeof(float));
        p += run;
    }
}

int64_t AudioRing::endSample() const {
    std::lock_guard lock(mu_);
    return end_;
}

void AudioRing::silenceFrom(int64_t firstSample) {
    std::lock_guard lock(mu_);
    for (int64_t p = std::max({firstSample, end_ - cap_, int64_t(0)}); p < end_;) {
        const int64_t slot = p % cap_, run = std::min(end_ - p, cap_ - slot);
        std::fill_n(buf_.data() + slot * 2, run * 2, 0.0f);
        p += run;
    }
}

} // namespace vt::media
