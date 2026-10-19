#include "vt/media/time.h"

#include <atomic>
#include <chrono>

extern "C" {
#include <libavutil/mathematics.h>
}

namespace vt::media {

SessionTime toSession(int64_t pts, AVRational tb) {
    return av_rescale_q_rnd(pts, tb, kSessionTb,
                            static_cast<AVRounding>(AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX));
}

struct SessionClock::State {
    bool isManual = false;
    std::chrono::steady_clock::time_point origin = std::chrono::steady_clock::now();
    std::atomic<SessionTime> manualNow{0};
};

SessionClock SessionClock::realtime() { return SessionClock(std::make_shared<State>()); }

SessionClock SessionClock::manual() {
    auto s = std::make_shared<State>();
    s->isManual = true;
    return SessionClock(std::move(s));
}

SessionTime SessionClock::now() const {
    if (state_->isManual) return state_->manualNow.load();
    // ns -> 1/240000 s is ns * 3 / 12500. Split so the multiply cannot overflow int64.
    const int64_t ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                           std::chrono::steady_clock::now() - state_->origin).count();
    return (ns / 12500) * 3 + (ns % 12500) * 3 / 12500;
}

void SessionClock::set(SessionTime t) {
    if (state_->isManual) state_->manualNow.store(t);
}

} // namespace vt::media
