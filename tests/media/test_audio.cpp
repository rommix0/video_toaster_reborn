#include "catch_amalgamated.hpp"
#include "vt/media/audio_convert.h"

#include <algorithm>
#include <cmath>
#include <numbers>

using namespace vt::media;

namespace {

float peak(std::span<const float> v) {
    float p = 0;
    for (float x : v) p = std::max(p, std::fabs(x));
    return p;
}

double rms(std::span<const float> interleaved, int channel) {
    double sum = 0;
    size_t n = interleaved.size() / 2;
    for (size_t i = 0; i < n; ++i) sum += double(interleaved[i * 2 + channel]) * interleaved[i * 2 + channel];
    return std::sqrt(sum / double(n));
}

// Zero crossings of one channel: steps between a negative and a non-negative sample.
int crossings(std::span<const float> interleaved, int channel) {
    int n = 0;
    for (size_t i = 1; i < interleaved.size() / 2; ++i)
        n += (interleaved[(i - 1) * 2 + channel] < 0) != (interleaved[i * 2 + channel] < 0);
    return n;
}

FramePtr audioFrame(AVSampleFormat fmt, int rate, const AVChannelLayout& layout, int samples) {
    FramePtr f(av_frame_alloc());
    f->format = fmt;
    f->sample_rate = rate;
    f->nb_samples = samples;
    REQUIRE(av_channel_layout_copy(&f->ch_layout, &layout) >= 0);
    REQUIRE(av_frame_get_buffer(f.get(), 0) >= 0);
    return f;
}

} // namespace

TEST_CASE("44.1 kHz mono sine becomes 48 kHz stereo at the same pitch") {
    constexpr int kIn = 44100;
    const AVChannelLayout mono = AV_CHANNEL_LAYOUT_MONO;
    FramePtr f = audioFrame(AV_SAMPLE_FMT_S16, kIn, mono, kIn);
    auto* pcm = reinterpret_cast<int16_t*>(f->data[0]);
    for (int i = 0; i < kIn; ++i)
        pcm[i] = int16_t(std::lround(20000.0 * std::sin(2 * std::numbers::pi * 1000.0 * i / kIn)));

    AudioConverter conv;
    std::span<const float> out = conv.convert(*f);

    REQUIRE(out.size() % 2 == 0);
    CHECK(std::abs(int(out.size() / 2) - 48000) <= 32);
    for (size_t i = 0; i < out.size() / 2; ++i) REQUIRE(out[i * 2] == out[i * 2 + 1]);
    CHECK(std::abs(crossings(out, 0) - 2000) <= 4);
    const float ratio = peak(out) / (20000.0f / 32768.0f);
    CHECK(ratio >= 0.99f);
    CHECK(ratio <= 1.01f);
}

TEST_CASE("5.1 downmix is stereo and not silent") {
    constexpr int kRate = 48000;
    const AVChannelLayout fiveOne = AV_CHANNEL_LAYOUT_5POINT1;
    FramePtr f = audioFrame(AV_SAMPLE_FMT_FLTP, kRate, fiveOne, kRate);
    for (int c = 0; c < 6; ++c) {
        auto* p = reinterpret_cast<float*>(f->extended_data[c]);
        const bool tone = c == 0 || c == 1 || c == 2; // FL, FR, FC
        for (int i = 0; i < kRate; ++i)
            p[i] = tone ? float(0.5 * std::sin(2 * std::numbers::pi * 440.0 * i / kRate)) : 0.0f;
    }

    AudioConverter conv;
    std::span<const float> out = conv.convert(*f);

    CHECK(std::abs(int(out.size() / 2) - 48000) <= 32);
    CHECK(rms(out, 0) > 0.1);
    CHECK(rms(out, 1) > 0.1);
}

TEST_CASE("8SVX effect sound resamples to its duration") {
    vt::Sound s;
    s.rate = 8363;
    s.channels = 1;
    s.pcm.resize(8363);
    for (size_t i = 0; i < s.pcm.size(); ++i)
        s.pcm[i] = int16_t(std::lround(16000.0 * std::sin(2 * std::numbers::pi * 440.0 * double(i) / 8363.0)));

    AudioConverter conv;
    std::span<const float> out = conv.convert(s);

    CHECK(std::abs(int(out.size() / 2) - 48000) <= 32);
    CHECK(peak(out) > 0.4f);
}

TEST_CASE("streaming frames keep the resampler's state and rebuild it on a format change") {
    constexpr int kIn = 44100, kChunk = 441;
    const AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
    AudioConverter conv;
    size_t total = 0;
    // 1 s in 10 ms frames: the output adds up to a second, with no gap or repeat at the joins.
    for (int start = 0; start < kIn; start += kChunk) {
        FramePtr f = audioFrame(AV_SAMPLE_FMT_FLT, kIn, stereo, kChunk);
        auto* p = reinterpret_cast<float*>(f->data[0]);
        for (int i = 0; i < kChunk; ++i)
            p[i * 2] = p[i * 2 + 1] = float(0.5 * std::sin(2 * std::numbers::pi * 1000.0 * (start + i) / kIn));
        total += conv.convert(*f).size() / 2;
    }
    CHECK(std::abs(int(total) - 48000) <= 64);

    // A different rate and layout rebuilds the resampler and still converts.
    const AVChannelLayout mono = AV_CHANNEL_LAYOUT_MONO;
    FramePtr g = audioFrame(AV_SAMPLE_FMT_S16, 24000, mono, 2400);
    std::span<const float> out = conv.convert(*g);
    CHECK(out.size() / 2 > 4000);
}

TEST_CASE("ring: gaps are silence, overlaps replace, old data expires") {
    AudioRing ring;
    CHECK(ring.endSample() == 0);

    std::vector<float> a(800 * 2, 0.5f), b(800 * 2, 0.25f);
    ring.write(0, a);
    ring.write(1600, b);
    CHECK(ring.endSample() == 2400);

    std::vector<float> got(2400 * 2);
    ring.read(0, got);
    for (int i = 0; i < 2400; ++i) {
        const float want = i < 800 ? 0.5f : i < 1600 ? 0.0f : 0.25f;
        REQUIRE(got[i * 2] == want);
        REQUIRE(got[i * 2 + 1] == want);
    }

    std::vector<float> c(200 * 2, -1.0f);
    ring.write(700, c);
    ring.read(0, got);
    for (int i = 700; i < 900; ++i) REQUIRE(got[i * 2] == -1.0f);
    CHECK(got[699 * 2] == 0.5f);
    CHECK(got[900 * 2] == 0.0f);
    CHECK(ring.endSample() == 2400);

    // Not yet written, and before the start: silence.
    std::vector<float> edge(200 * 2, 9.0f);
    ring.read(2300, edge);
    CHECK(edge[0] == 0.25f);
    CHECK(edge[99 * 2] == 0.25f);
    CHECK(edge[100 * 2] == 0.0f);
    ring.read(-100, edge);
    CHECK(edge[0] == 0.0f);
    CHECK(edge[99 * 2 + 1] == 0.0f);
    CHECK(edge[100 * 2] == 0.5f);

    AudioRing small(4800);
    small.write(0, a);
    small.write(10000, a);
    std::vector<float> old(800 * 2, 9.0f);
    small.read(0, old);
    CHECK(peak(old) == 0.0f);
    small.read(10000, old);
    CHECK(old[0] == 0.5f);
    CHECK(old[799 * 2] == 0.5f);
    // The gap before the new write reads as silence even where an older lap's slots are reused.
    small.read(9000, old);
    CHECK(peak(old) == 0.0f);
}

TEST_CASE("ring: a write wrapping the end of the buffer reads back whole") {
    AudioRing ring(1000);
    std::vector<float> ramp(600 * 2);
    for (int i = 0; i < 600; ++i) ramp[i * 2] = ramp[i * 2 + 1] = float(i);
    ring.write(0, ramp);
    ring.write(600, ramp);
    ring.write(1200, ramp); // wraps; window is [800, 1800)
    std::vector<float> got(100 * 2);
    ring.read(900, got);
    for (int i = 0; i < 100; ++i) REQUIRE(got[i * 2] == float(300 + i));
    ring.read(750, got);
    CHECK(got[0] == 0.0f);
    CHECK(got[49 * 2] == 0.0f);
    CHECK(got[50 * 2] == float(200)); // sample 800 came from the second write (600 + 200)
}
