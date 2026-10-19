#include "catch_amalgamated.hpp"
#include "vt/media/mixer.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <numbers>
#include <thread>

#include "vt/media/time.h"

using namespace vt::media;
using vt::Source;

namespace {

struct Rig {
    AudioMixer mixer;
    std::vector<float> in1, in2, in3, in4, out;
    int64_t tick = 0;

    Rig() { setDc(0.5f, 0.25f); }
    void setDc(float a, float b) {
        in1.assign(2 * 801, a);
        in2.assign(2 * 801, b);
        in3.assign(2 * 801, 0.0f);
        in4.assign(2 * 801, 0.0f);
    }
    // Runs one tick; the result is in `out`.
    const std::vector<float>& run(const MixState& st, std::shared_ptr<const vt::Sound> sound = nullptr) {
        const size_t n = static_cast<size_t>(samplesInTick(tick++));
        out.assign(2 * n, 7.0f);
        std::array<std::span<const float>, 4> in{std::span(in1).first(2 * n), std::span(in2).first(2 * n),
                                                 std::span(in3).first(2 * n), std::span(in4).first(2 * n)};
        mixer.process(st, in, std::move(sound), out);
        return out;
    }
};

MixState state(Source main, Source preview, int tbar = 0, bool transition = false, bool chroma = false,
               bool overlay = false) {
    return {main, preview, tbar, transition, chroma, overlay};
}

} // namespace

TEST_CASE("MixState::of reads the Switcher") {
    vt::Switcher sw;
    sw.select(vt::Bus::Main, Source::In3);
    sw.select(vt::Bus::Preview, Source::DV2);
    sw.setTBar(100);
    MixState s = MixState::of(sw);
    CHECK(s.main == Source::In3);
    CHECK(s.preview == Source::DV2);
    CHECK(s.tbar == 100);
    CHECK(s.transition);
    CHECK_FALSE(s.chroma);
    CHECK_FALSE(s.overlay);
}

TEST_CASE("no transition: Main only") {
    Rig r;
    r.run(state(Source::In1, Source::In2)); // settling tick (gains ramp up from silence)
    const auto& out = r.run(state(Source::In1, Source::In2));
    for (float s : out) CHECK(s == Catch::Approx(0.5f).margin(1e-6));
}

TEST_CASE("mid transition is equal power") {
    Rig r;
    const MixState st = state(Source::In1, Source::In2, 256, true);
    r.run(st);
    const auto& out = r.run(st);
    const double p = 256 / 511.0 * std::numbers::pi / 2;
    const double want = 0.5 * std::cos(p) + 0.25 * std::sin(p);
    CHECK(out[out.size() - 2] == Catch::Approx(want).margin(1e-5));
    CHECK(out[out.size() - 1] == Catch::Approx(want).margin(1e-5));
}

TEST_CASE("same source on both buses keeps its level") {
    for (int tbar : {0, 128, 256, 511}) {
        Rig r;
        const MixState st = state(Source::In1, Source::In1, tbar, true);
        r.run(st);
        const auto& out = r.run(st);
        for (float s : out) CHECK(s == Catch::Approx(0.5f).margin(1e-6));
    }
}

TEST_CASE("BKG and DV sources are silent") {
    Rig r;
    const MixState st = state(Source::Bkg, Source::DV1, 256, true);
    r.run(st);
    for (float s : r.run(st)) CHECK(s == 0.0f);
    // A silent source against an input: only the input's share remains.
    const MixState st2 = state(Source::DV2, Source::In2, 511, true);
    r.run(st2);
    CHECK(r.run(st2).back() == Catch::Approx(0.25f).margin(1e-6));
}

TEST_CASE("a cut ramps over one tick") {
    Rig r;
    r.run(state(Source::In1, Source::In2));
    const auto out = r.run(state(Source::In2, Source::In1)); // the cut: Main In1 -> In2
    const size_t n = out.size() / 2;
    CHECK(out[0] == Catch::Approx(0.5f).margin(0.001));
    CHECK(out[2 * (n - 1)] == Catch::Approx(0.25f).margin(1e-6));
    for (size_t j = 1; j < n; ++j) {
        const float step = out[2 * j] - out[2 * (j - 1)];
        CHECK(step <= 0.0f);
        CHECK(std::fabs(step) < 0.001f);
    }
    // And the right channel is the same.
    CHECK(out[1] == out[0]);
    CHECK(out[2 * n - 1] == out[2 * (n - 1)]);
}

TEST_CASE("ChromaFX engaged follows Preview, overlay follows Main") {
    {
        Rig r;
        const MixState st = state(Source::In1, Source::In2, 300, true, /*chroma*/ true);
        r.run(st);
        for (float s : r.run(st)) CHECK(s == Catch::Approx(0.25f).margin(1e-6));
    }
    {
        Rig r;
        const MixState st = state(Source::In1, Source::In2, 300, true, false, /*overlay*/ true);
        r.run(st);
        for (float s : r.run(st)) CHECK(s == Catch::Approx(0.5f).margin(1e-6));
    }
}

namespace {

std::shared_ptr<const vt::Sound> testSound() {
    auto s = std::make_shared<vt::Sound>();
    s->rate = 8363;
    s->channels = 1;
    s->pcm.assign(4181, static_cast<int16_t>(0.5 * 32767));
    return s;
}

} // namespace

TEST_CASE("effect sound starts at its tick and runs to its end") {
    Rig r;
    r.setDc(0, 0);
    auto snd = testSound();
    const MixState a = state(Source::In1, Source::In2);
    const MixState b = state(Source::In3, Source::In4, 200, true); // "a different effect" mid-play

    int64_t loud = 0, first = -1, total = 0;
    for (int k = 0; k < 40; ++k) {
        const auto& out = r.run(k < 10 ? a : b, k == 0 ? snd : nullptr);
        for (size_t j = 0; j < out.size() / 2; ++j) {
            if (std::fabs(out[2 * j]) > 0.25f) {
                if (first < 0) first = total + int64_t(j);
                ++loud;
            }
        }
        total += int64_t(out.size() / 2);
    }
    CHECK(first == 0);
    CHECK(std::llabs(loud - 24000) <= 32);
    // The converted sound is cached: a second start of the same Sound plays the same length.
    int64_t loud2 = 0;
    for (int k = 0; k < 40; ++k)
        for (float s : r.run(a, k == 0 ? snd : nullptr))
            if (std::fabs(s) > 0.25f) ++loud2;
    CHECK(loud2 / 2 == loud);
}

TEST_CASE("overlapping effect sounds sum") {
    Rig r;
    r.setDc(0, 0);
    auto s1 = testSound(), s2 = testSound();
    const MixState st = state(Source::In1, Source::In2);
    r.run(st, s1);
    r.run(st); // settle past the resampler's start
    const float one = r.run(st)[100];
    r.run(st, s2);
    const auto& two = r.run(st);
    CHECK(two[100] == Catch::Approx(one + one).margin(0.02));
}

TEST_CASE("faders, mute and meters") {
    Rig r;
    const MixState st = state(Source::In1, Source::In2);
    r.run(st);

    r.mixer.setFaderDb(Channel::In1, -6.0206f);
    r.run(st); // ramp tick
    for (float s : r.run(st)) CHECK(s == Catch::Approx(0.25f).margin(1e-4));

    r.mixer.setMute(Channel::In1, true);
    r.run(st);
    for (float s : r.run(st)) CHECK(s == 0.0f);
    r.mixer.setMute(Channel::In1, false);
    r.mixer.setFaderDb(Channel::In1, 0.0f);

    r.mixer.setFaderDb(Channel::Master, -96.0f);
    r.run(st);
    for (float s : r.run(st)) CHECK(s == 0.0f);
    r.mixer.setFaderDb(Channel::Master, 0.0f);

    // Full-scale 1 kHz sine on In1.
    for (size_t i = 0; i < r.in1.size() / 2; ++i)
        r.in1[2 * i] = r.in1[2 * i + 1] = static_cast<float>(std::sin(2 * std::numbers::pi * 1000.0 * double(i) / 48000.0));
    r.run(st);
    r.run(st);
    const auto m = r.mixer.meters();
    const Meter in1 = m[size_t(Channel::In1)], master = m[size_t(Channel::Master)];
    CHECK(in1.peakDb == Catch::Approx(0.0f).margin(0.1));
    CHECK(in1.rmsDb == Catch::Approx(-3.01f).margin(0.1));
    CHECK(master.peakDb == Catch::Approx(0.0f).margin(0.1));
    CHECK(master.rmsDb == Catch::Approx(-3.01f).margin(0.1));
    CHECK(m[size_t(Channel::In3)].peakDb == -INFINITY);
    CHECK(m[size_t(Channel::EffectSound)].rmsDb == -INFINITY);

    // Meters are post-fader.
    r.mixer.setFaderDb(Channel::In1, -6.0206f);
    r.run(st);
    r.run(st);
    CHECK(r.mixer.meters()[size_t(Channel::In1)].peakDb == Catch::Approx(-6.02f).margin(0.1));
}

TEST_CASE("master clamps to full scale, short or missing inputs are silence") {
    Rig r;
    r.setDc(1.0f, 1.0f);
    const MixState st = state(Source::In1, Source::In2, 256, true);
    r.mixer.setFaderDb(Channel::Master, 12.0f);
    r.run(st);
    for (float s : r.run(st)) CHECK(std::fabs(s) <= 1.0f);

    std::vector<float> out(2 * 800, 5.0f);
    std::array<std::span<const float>, 4> none{};
    r.mixer.process(state(Source::In1, Source::In1), none, nullptr, out);
    r.mixer.process(state(Source::In1, Source::In1), none, nullptr, out);
    for (float s : out) CHECK(s == 0.0f);
}

TEST_CASE("setters from another thread while process runs") {
    Rig r;
    const MixState st = state(Source::In1, Source::In2);
    std::atomic<bool> stop{false};
    std::thread t([&] {
        for (int i = 0; i < 20000; ++i) {
            r.mixer.setFaderDb(Channel::In1, (i % 2) ? -6.0f : 0.0f);
            r.mixer.setMute(Channel::In2, i % 3 == 0);
            r.mixer.setFaderDb(Channel::Master, (i % 5) ? -1.0f : 0.0f);
            (void)r.mixer.meters();
        }
        stop = true;
    });
    int ticks = 0;
    bool inRange = true;
    while (!stop || ticks < 50) {
        for (float s : r.run(st)) inRange = inRange && std::fabs(s) <= 1.0f;
        ++ticks;
    }
    t.join();
    CHECK(inRange);

    r.mixer.setFaderDb(Channel::In1, -6.0206f);
    r.mixer.setMute(Channel::In2, true);
    r.mixer.setFaderDb(Channel::Master, 0.0f);
    r.run(st);
    for (float s : r.run(st)) CHECK(s == Catch::Approx(0.25f).margin(1e-4));
    r.mixer.setMute(Channel::In1, true);
    r.run(st);
    for (float s : r.run(st)) CHECK(s == 0.0f);
}
