#pragma once
// Session time: one integer timeline for everything the media layer schedules. One unit is
// 1/240000 s, so a 59.94 Hz tick (1001/60000 s) is exactly 4004 units and 48 kHz audio puts
// 4004/5 samples in a tick. All three standard rates (23.976/29.97/59.94) land on whole units.
#include <cstdint>
#include <memory>

extern "C" {
#include <libavutil/rational.h>
}

namespace vt::media {

using SessionTime = int64_t;
inline constexpr AVRational kSessionTb{1, 240000};

// Start of tick n (n = 0, 1, 2, ...).
constexpr SessionTime tickTime(int64_t n) { return n * 4004; }
// 48 kHz samples before tick n: n * 4004 / 5, rounded down; ticks alternate 800 / 801 samples
// with no drift.
constexpr int64_t samplesBefore(int64_t n) { return n * 4004 / 5; }
constexpr int samplesInTick(int64_t n) { return static_cast<int>(samplesBefore(n + 1) - samplesBefore(n)); }

// A stream timestamp in `tb` units converted to session time (rounded to nearest, ties away
// from zero; AV_NOPTS_VALUE and the int64 limits pass through unchanged).
SessionTime toSession(int64_t pts, AVRational tb);

// The session's notion of "now". A cheap handle: copies share one origin, now() is thread-safe.
class SessionClock {
public:
    // Follows steady_clock; zero at the moment of construction.
    static SessionClock realtime();
    // Stands still until set(); for tests and offline rendering.
    static SessionClock manual();

    SessionTime now() const;
    // Sets a manual clock's time. Ignored on a realtime clock.
    void set(SessionTime t);

private:
    struct State;
    explicit SessionClock(std::shared_ptr<State> s) : state_(std::move(s)) {}
    std::shared_ptr<State> state_;
};

} // namespace vt::media
