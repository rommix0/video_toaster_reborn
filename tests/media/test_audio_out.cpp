#include "catch_amalgamated.hpp"
#include "vt/log.h"
#include "vt/media/audio_out.h"
#include "vt/media/time.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <numbers>
#include <string>
#include <thread>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <mmsystem.h>

using namespace vt::media;

namespace {

constexpr int kTarget = 1700;   // frames: about 35 ms

// The FIFO between a program loop that pushes a tick (800 or 801 frames) every 1001/60000 s and a
// device that pulls 480 frames every 10 ms on its own clock, `skewPpm` faster than the loop's. The
// controller is called once per pull; the pull takes ratio * 480 frames. Returns the FIFO level
// before each pull, with whether it ever ran dry or overflowed.
struct Simulation {
    std::vector<int> levels;   // one per pull
    bool dry = false, full = false;
};

Simulation simulate(double skewPpm, double seconds) {
    DriftController drift(kTarget);
    Simulation sim;
    double fifo = kTarget + 800, carry = 0;   // where the render thread's priming can start it, at worst
    const double loopPeriod = 1001.0 / 60000.0, devicePeriod = 0.010 / (1.0 + skewPpm * 1e-6);
    double nextTick = 0, nextPull = devicePeriod;
    int64_t tick = 0;
    while (std::min(nextTick, nextPull) < seconds) {
        if (nextTick <= nextPull) {
            fifo += samplesInTick(tick);
            nextTick = double(++tick) * loopPeriod;
            sim.full = sim.full || fifo > 48000;
        } else {
            sim.levels.push_back(int(fifo));
            carry += 480 * drift.ratio(int(fifo));
            const double take = std::floor(carry);
            carry -= take;
            sim.dry = sim.dry || fifo < take;
            fifo = std::max(fifo - take, 0.0);
            nextPull += devicePeriod;
        }
    }
    return sim;
}

} // namespace

TEST_CASE("DriftController pushes the FIFO back toward its target", "[audio_out]") {
    SECTION("above target the ratio is above 1, below target it is below") {
        DriftController up(kTarget), down(kTarget), on(kTarget);
        CHECK(up.ratio(kTarget + 1000) > 1.0);
        CHECK(down.ratio(kTarget - 1000) < 1.0);
        CHECK(on.ratio(kTarget) == Catch::Approx(1.0).margin(1e-12));
    }
    SECTION("the ratio never leaves 1 +- 0.002") {
        DriftController c(kTarget);
        for (int level : {0, 48000, 100000, 0, kTarget, 5, 47999}) {
            for (int i = 0; i < 500; ++i) {
                const double r = c.ratio(level);
                CHECK(std::fabs(r - 1.0) <= 0.002 + 1e-12);
            }
        }
        DriftController high(kTarget);
        for (int i = 0; i < 500; ++i) high.ratio(48000);
        CHECK(high.ratio(48000) == Catch::Approx(1.002).margin(1e-12));
        DriftController low(kTarget);
        for (int i = 0; i < 500; ++i) low.ratio(0);
        CHECK(low.ratio(0) == Catch::Approx(0.998).margin(1e-12));
    }
}

TEST_CASE("DriftController keeps the FIFO within 10 ms of target under clock skew", "[audio_out]") {
    for (const double skew : {-300.0, 0.0, 300.0}) {
        CAPTURE(skew);
        const Simulation sim = simulate(skew, 600);   // ten minutes
        REQUIRE(sim.levels.size() > 55000);
        CHECK_FALSE(sim.dry);
        CHECK_FALSE(sim.full);
        // After a 30 s settling time (the start is up to 800 frames off, and the correction closes a
        // time constant of about 2.4 s) the level jumps by a tick per push and falls by a pull per
        // callback, so look at one-second means ...
        int worstMean = 0, worstEdge = 0;
        for (size_t i = 3000; i + 100 <= sim.levels.size(); i += 100) {
            double sum = 0;
            for (size_t k = i; k < i + 100; ++k) sum += sim.levels[k];
            worstMean = std::max(worstMean, int(std::lround(std::fabs(sum / 100 - kTarget))));
        }
        // ... and at every pull.
        for (size_t i = 3000; i < sim.levels.size(); ++i) worstEdge = std::max(worstEdge, std::abs(sim.levels[i] - kTarget));
        INFO("worst one-second mean off target: " << worstMean << " frames, worst single level: " << worstEdge);
        CHECK(worstMean <= 480);   // 10 ms
        CHECK(worstEdge <= 480);
    }
}

TEST_CASE("listOutputDevices never throws", "[audio_out]") {
    std::string error = "x";
    std::vector<OutputDevice> devices;
    REQUIRE_NOTHROW(devices = listOutputDevices(&error));
    if (error.empty()) {
        for (const OutputDevice& d : devices) {
            CHECK_FALSE(d.id.empty());
            CHECK_FALSE(d.name.empty());
        }
        CHECK(std::count_if(devices.begin(), devices.end(), [](const OutputDevice& d) { return d.isDefault; }) <= 1);
    } else {
        CHECK(devices.empty());
    }
    REQUIRE_NOTHROW(listOutputDevices());
}

TEST_CASE("AudioOutput that never started", "[audio_out]") {
    AudioOutput out;
    const vt::Frame frame;
    const std::vector<float> tick(800 * 2, 0.5f);
    out.onProgram(0, frame, tick);   // ignored, and does not crash
    auto st = out.status();
    CHECK_FALSE(st.running);
    CHECK(st.error.empty());
    CHECK(st.underruns == 0);
    CHECK(st.overruns == 0);
    out.stop();                      // does nothing

    std::string logged;
    vt::setLogSink([&](std::string_view m) { logged = m; });
    struct Restore { ~Restore() { vt::setLogSink({}); } } restore;
    AudioOutputSettings bogus;
    bogus.deviceId = "no such device";
    std::string error;
    CHECK_FALSE(out.start(bogus, &error));
    CHECK_FALSE(error.empty());
    st = out.status();
    CHECK_FALSE(st.running);
    CHECK(st.error == error);
    CHECK(logged.find(error) != std::string::npos);
    out.stop();
}

TEST_CASE("AudioOutput plays the program audio on the default device", "[audio_out][live]") {
    timeBeginPeriod(1);
    struct Period { ~Period() { timeEndPeriod(1); } } period;

    AudioOutput out;
    std::string error;
    REQUIRE(out.start({}, &error));
    REQUIRE(error.empty());
    REQUIRE(out.status().running);

    // Two seconds of 1 kHz at the loop's real rate (tick n is due at n * 1001/60 ms).
    const vt::Frame frame;
    const auto t0 = std::chrono::steady_clock::now();
    std::vector<float> tick;
    int64_t sample = 0;
    for (int64_t n = 0; n < 120; ++n) {
        std::this_thread::sleep_until(t0 + std::chrono::microseconds(n * 1001000 / 60));
        const int64_t frames = samplesInTick(n);
        tick.resize(size_t(frames) * 2);
        for (int64_t i = 0; i < frames; ++i, ++sample) {
            const float v = 0.2f * std::sin(2.0f * std::numbers::pi_v<float> * 1000.0f * float(sample % 48) / 48000.0f);
            tick[size_t(i) * 2] = tick[size_t(i) * 2 + 1] = v;
        }
        out.onProgram(n, frame, tick);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    const auto st = out.status();
    INFO("underruns " << st.underruns << ", overruns " << st.overruns << ", drift " << st.driftPpm << " ppm");
    CHECK(st.running);
    CHECK(st.error.empty());
    CHECK(st.underruns <= 2);
    CHECK(st.overruns == 0);
    CHECK(std::fabs(st.driftPpm) <= 2001.0);   // DriftController's limit

    const auto stopStart = std::chrono::steady_clock::now();
    out.stop();
    CHECK(std::chrono::steady_clock::now() - stopStart < std::chrono::seconds(1));
    CHECK_FALSE(out.status().running);
}
