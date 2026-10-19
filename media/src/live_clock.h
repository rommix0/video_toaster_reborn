#pragma once
// The clock mapping of one live capture source (internal to vt_media). Its packets carry
// timestamps on the device's clock; the session needs them on the session clock. All the
// streams of one source (a camera's video and its microphone) share one mapping, so they stay in
// sync with each other. Pure arithmetic on (timestamp, arrival) pairs, so it is tested without
// devices.
//
// The mapping is session time = base() + ts. The first packet anchors it at its arrival. A packet
// that arrives more than 2 ms before its mapped time shows that the mapping is too late (an
// early packet was stale, or the device clock runs fast): the mapping moves earlier to it at
// once, so it follows the lowest latency seen. Arriving late is normal for some streams (a
// microphone delivers each buffer when it is full, ~50 ms after its timestamp, while the camera's
// frames come at once), so the mapping only moves later when every packet of a whole window
// (0.5 s) arrived more than `lateLimit` after its mapped time; it then moves to the least late of
// them. The input reads `presentationDelay` behind the clock, so a late limit of half that delay
// keeps every packet in time for its reader.
#include <algorithm>
#include <cstdint>

#include "vt/media/time.h"

namespace vt::media {

class LiveClock {
public:
    static constexpr SessionTime kEarlyMargin = 480;  // 2 ms of arrival jitter that does not move the mapping
    static constexpr SessionTime kWindow = 120000;    // 0.5 s that must all be late before it moves later

    enum class Change { None, Anchored, Earlier, Later };

    LiveClock() = default; // late limit 12000 (50 ms: half the default 100 ms delay)
    explicit LiveClock(SessionTime lateLimit) : lateLimit_(lateLimit) {}

    // A packet stamped `ts` (device clock, in session units) arrived at `arrival` (session
    // clock): updates the mapping and says how it changed (shift() says by how much).
    Change update(SessionTime ts, SessionTime arrival) {
        if (!anchored_) {
            anchored_ = true;
            settled_ = false;
            base_ = arrival - ts;
            shift_ = 0;
            windowStart_ = arrival;
            windowMin_ = 0;
            return Change::Anchored;
        }
        const SessionTime late = arrival - (base_ + ts);
        if (late < -kEarlyMargin) {
            base_ += late;
            shift_ = late;
            windowMin_ = 0; // this packet, now on time, is the least late of the window
            return Change::Earlier;
        }
        windowMin_ = std::min(windowMin_, late);
        if (arrival - windowStart_ < kWindow) return Change::None;
        const SessionTime least = windowMin_;
        settled_ = true;
        windowStart_ = arrival;
        windowMin_ = INT64_MAX;
        if (least <= lateLimit_) return Change::None;
        base_ += least;
        shift_ = least;
        return Change::Later;
    }

    bool anchored() const { return anchored_; }
    SessionTime base() const { return base_; }
    SessionTime shift() const { return shift_; } // the last change of base(), in session units
    SessionTime lateLimit() const { return lateLimit_; }
    // False during the first window after the anchor, while stale first packets settle.
    bool settled() const { return settled_; }

private:
    SessionTime lateLimit_ = 12000;
    bool anchored_ = false, settled_ = false;
    SessionTime base_ = 0, shift_ = 0;
    SessionTime windowStart_ = 0;      // arrival that opened the current window
    SessionTime windowMin_ = INT64_MAX; // least lateness in the current window
};

} // namespace vt::media
