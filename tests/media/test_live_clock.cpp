#include "catch_amalgamated.hpp"
#include "live_clock.h"

#include <algorithm>
#include <vector>

using namespace vt::media;
using Change = LiveClock::Change;

namespace {

constexpr SessionTime kMs = 240;            // session units per millisecond
constexpr SessionTime kFrame = 8008;        // a 29.97 fps frame
constexpr SessionTime kLate = 50 * kMs;     // the late limit for a 100 ms presentation delay

} // namespace

TEST_CASE("live clock: the first packet anchors the mapping at its arrival") {
    LiveClock c(kLate);
    CHECK_FALSE(c.anchored());
    CHECK(c.update(900000, 5000) == Change::Anchored); // device clock at 3.75 s, session at 20.8 ms
    CHECK(c.anchored());
    CHECK(c.base() == 5000 - 900000);
    // Packets that keep to the mapping, give or take the 2 ms margin, change nothing.
    CHECK(c.update(900000 + kFrame, 5000 + kFrame) == Change::None);
    CHECK(c.update(900000 + 2 * kFrame, 5000 + 2 * kFrame - 2 * kMs) == Change::None);  // 2 ms early: jitter
    CHECK(c.update(900000 + 3 * kFrame, 5000 + 3 * kFrame + 30 * kMs) == Change::None); // 30 ms late: in time
    CHECK(c.base() == 5000 - 900000);
}

TEST_CASE("live clock: a packet with lower latency moves the mapping earlier at once") {
    LiveClock c(kLate);
    // The first packet was stale: it arrived 340 ms after it was captured.
    c.update(0, 100000);
    // The next arrives only 20 ms after its timestamp's place on the old mapping minus 340 ms.
    const SessionTime arrival = 100000 + kFrame - 320 * kMs;
    CHECK(c.update(kFrame, arrival) == Change::Earlier);
    CHECK(c.base() == arrival - kFrame);
    CHECK(c.shift() == -320 * kMs);
    CHECK_FALSE(c.settled()); // still in the first window
    // Just past the margin moves it too; at the margin it stays.
    const SessionTime b = c.base();
    CHECK(c.update(2 * kFrame, b + 2 * kFrame - LiveClock::kEarlyMargin) == Change::None);
    CHECK(c.update(3 * kFrame, b + 3 * kFrame - LiveClock::kEarlyMargin - 1) == Change::Earlier);
    CHECK(c.base() == b - LiveClock::kEarlyMargin - 1);
}

TEST_CASE("live clock: it moves later only when a whole window arrived too late") {
    // Packets every frame, all exactly `late` after the mapping, for longer than one window.
    const auto run = [](SessionTime late, std::vector<Change>& changes) {
        LiveClock c(kLate);
        c.update(0, 0);
        for (int k = 1; k <= 40; ++k) changes.push_back(c.update(k * kFrame, k * kFrame + late));
        return c;
    };
    {
        std::vector<Change> changes;
        const LiveClock c = run(kLate, changes); // at the limit: in time
        CHECK(std::count(changes.begin(), changes.end(), Change::None) == 40);
        CHECK(c.base() == 0);
    }
    {
        std::vector<Change> changes;
        const LiveClock c = run(kLate + 1, changes); // one unit past it
        // The first window holds the anchor, on time by definition: it closes unchanged at the
        // first arrival 0.5 s on (packet 14, arriving 124113). The second, every packet late,
        // closes 0.5 s after that (packet 29) and moves the mapping by the least lateness.
        for (int k = 1; k <= 40; ++k) {
            INFO("packet " << k);
            CHECK(changes[size_t(k - 1)] == (k == 29 ? Change::Later : Change::None));
        }
        CHECK(c.base() == kLate + 1);
        CHECK(c.shift() == kLate + 1);
        CHECK(c.settled());
    }
}

TEST_CASE("live clock: one packet in time in a window holds the mapping") {
    LiveClock c(kLate);
    c.update(0, 0);
    for (int k = 1; k <= 40; ++k) {
        // Every packet 80 ms late but every tenth, which is on time: the latency floor is still 0.
        const SessionTime late = k % 10 == 0 ? 0 : 80 * kMs;
        CHECK(c.update(k * kFrame, k * kFrame + late) == Change::None);
    }
    CHECK(c.base() == 0);
}

TEST_CASE("live clock: video and audio of one source share the mapping") {
    // A camera's frames arrive at once; its microphone delivers 50 ms buffers, each ~55 ms after
    // the buffer's timestamp. Both on one device clock, so both use the camera's mapping: the
    // sound is not moved later, and nothing flaps.
    LiveClock c(kLate);
    const SessionTime dev = 7000000; // the device clock's reading at session time 0
    std::vector<Change> changes;
    SessionTime nextAudio = 0;
    for (SessionTime t = 0; t < 3 * 240000; t += kFrame) {
        changes.push_back(c.update(dev + t, t + 1 * kMs)); // a frame, 1 ms after capture
        while (nextAudio + 50 * kMs + 5 * kMs <= t) {     // the buffers completed by now
            changes.push_back(c.update(dev + nextAudio, nextAudio + 55 * kMs));
            nextAudio += 50 * kMs;
        }
    }
    CHECK(changes.front() == Change::Anchored);
    CHECK(std::count(changes.begin() + 1, changes.end(), Change::None) == std::ptrdiff_t(changes.size() - 1));
    CHECK(c.base() == 1 * kMs - dev);
    // A sound-only start is anchored by the sound; once the camera's first frame arrives with
    // lower latency, the shared mapping moves earlier for both.
    LiveClock d(kLate);
    CHECK(d.update(dev, 55 * kMs) == Change::Anchored);
    CHECK(d.update(dev + kFrame, kFrame + 1 * kMs) == Change::Earlier);
    CHECK(d.base() == 1 * kMs - dev);
    CHECK(d.update(dev + 50 * kMs, 105 * kMs) == Change::None); // the next buffer: late, but in time
}
