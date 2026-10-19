#include "vt/media/program_loop.h"

#include <algorithm>
#include <cassert>
#include <exception>
#include <string>

#include "vt/log.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <timeapi.h>

namespace vt::media {

namespace {

using Steady = std::chrono::steady_clock;
using Units = std::chrono::duration<int64_t, std::ratio<1, 240000>>; // session time units

Steady::duration toSteady(SessionTime t) { return std::chrono::duration_cast<Steady::duration>(Units(t)); }

constexpr size_t kTickFrames = 801;           // the most stereo frames in a tick
constexpr auto kLateAfter = std::chrono::milliseconds(1);
constexpr auto kStallAfter = std::chrono::seconds(1);

// Waits for absolute steady-clock deadlines: a high-resolution waitable timer, or (before
// Windows 10 1803) an ordinary one with the system timer at 1 ms.
class DeadlineTimer {
public:
    DeadlineTimer() {
        timer_ = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        if (!timer_) {
            periodSet_ = timeBeginPeriod(1) == TIMERR_NOERROR;
            timer_ = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
        }
    }
    ~DeadlineTimer() {
        if (timer_) CloseHandle(timer_);
        if (periodSet_) timeEndPeriod(1);
    }
    DeadlineTimer(const DeadlineTimer&) = delete;
    DeadlineTimer& operator=(const DeadlineTimer&) = delete;

    void waitUntil(Steady::time_point deadline) {
        for (;;) {
            const auto left = deadline - Steady::now();
            if (left <= Steady::duration::zero()) return;
            if (!timer_) {
                std::this_thread::sleep_until(deadline);
                continue;
            }
            // Relative due time in 100 ns units (negative = relative), recomputed from the
            // deadline on every wait so nothing accumulates.
            LARGE_INTEGER due;
            due.QuadPart = -std::max<int64_t>(1, std::chrono::duration_cast<std::chrono::duration<int64_t, std::ratio<1, 10000000>>>(left).count());
            if (!SetWaitableTimer(timer_, &due, 0, nullptr, nullptr, FALSE)) {
                std::this_thread::sleep_until(deadline);
                continue;
            }
            WaitForSingleObject(timer_, INFINITE);
        }
    }

private:
    HANDLE timer_ = nullptr;
    bool periodSet_ = false;
};

// Smallest multiple of `m` (> 0) that is >= x.
int64_t roundUpTo(int64_t x, int64_t m) {
    const int64_t r = ((x % m) + m) % m;
    return r == 0 ? x : x + (m - r);
}

// Smallest integer >= a / b (b > 0).
int64_t ceilDiv(int64_t a, int64_t b) { return a >= 0 ? (a + b - 1) / b : -((-a) / b); }

// Runs f; an exception is logged as "<what> failed: <reason>" and goes no further.
template <class F>
void guarded(const char* what, F&& f) {
    try {
        f();
    } catch (const std::exception& e) {
        logWarning(std::string("program loop: ") + what + " failed: " + e.what());
    } catch (...) {
        logWarning(std::string("program loop: ") + what + " failed");
    }
}

} // namespace

ProgramLoop::ProgramLoop(ProgramConfig cfg)
    : geometry_(cfg.geometry), delayMs_(std::max(0, cfg.presentationDelayMs)), configs_(std::move(cfg.inputs)) {
    for (auto& a : audio_) a.resize(kTickFrames * 2);
    mix_.resize(kTickFrames * 2);
    publish();   // the Switcher's defaults, until the first tick
}

ProgramLoop::~ProgramLoop() { stop(); }

void ProgramLoop::post(std::function<void(vt::Switcher&)> f) {
    std::lock_guard l(mu_);
    posted_.push_back(std::move(f));
}

void ProgramLoop::postChecked(std::function<bool(vt::Switcher&, std::string& why)> f) {
    post([this, f = std::move(f)](vt::Switcher& sw) {   // runs on the loop thread, which owns work_
        std::string why;
        if (f(sw, why)) {
            work_.lastRefusal.clear();
        } else {
            work_.lastRefusal = why.empty() ? "refused" : std::move(why);
        }
    });
}

void ProgramLoop::setInput(int index, const InputConfig& c) {
    assert(index >= 0 && index < 4);
    if (index < 0 || index >= 4) return;
    std::lock_guard l(mu_);
    configs_[size_t(index)] = c;
    reopen_[size_t(index)] = true;
}

void ProgramLoop::addSink(ProgramSink* s) {
    std::lock_guard l(sinkMu_);
    if (std::find(sinks_.begin(), sinks_.end(), s) == sinks_.end()) sinks_.push_back(s);
}

void ProgramLoop::removeSink(ProgramSink* s) {
    std::lock_guard l(sinkMu_);
    std::erase(sinks_, s);
}

void ProgramLoop::addMonitor(MonitorSink* m) {
    std::lock_guard l(monitorMu_);
    if (std::find(monitors_.begin(), monitors_.end(), m) == monitors_.end()) monitors_.push_back(m);
}

void ProgramLoop::removeMonitor(MonitorSink* m) {
    std::lock_guard l(monitorMu_);
    std::erase(monitors_, m);
}

SwitcherSnapshot ProgramLoop::switcherState() const {
    std::lock_guard l(snapMu_);
    return snapshot_;
}

// The effect's name is a string built on every call, so it is read again only after posted
// commands ran: the one way the effect can change.
void ProgramLoop::publish() {
    SwitcherSnapshot& w = work_;
    w.main = sw_.selected(vt::Bus::Main);
    w.preview = sw_.selected(vt::Bus::Preview);
    w.overlay = sw_.selected(vt::Bus::Overlay);
    w.tbar = sw_.tbar();
    w.autoRunning = sw_.autoRunning();
    w.transition = sw_.transitionActive();
    w.chroma = sw_.chromaEngaged();
    w.overlayEngaged = sw_.overlayEngaged();
    w.speed = sw_.speed();
    if (nameStale_) {
        nameStale_ = false;
        std::string name = sw_.effect().name();
        if (name != w.effectName) w.effectName = std::move(name);
    }
    w.effectColour = sw_.effectColour();
    w.background = sw_.background();
    w.tick = tick_ - 1;
    std::lock_guard l(snapMu_);
    snapshot_ = w;   // the strings reuse their capacity: no allocation unless a name or reason grew
}

InputStatus ProgramLoop::inputStatus(int index) const {
    if (index < 0 || index >= 4) return {};
    std::lock_guard l(mu_);
    const auto& in = inputs_[size_t(index)];
    return in ? in->status() : InputStatus{};
}

LoopStats ProgramLoop::stats() const {
    std::lock_guard l(mu_);
    return stats_;
}

void ProgramLoop::start() {
    std::lock_guard c(controlMu_);
    if (thread_.joinable()) return;
    prepare(true);
    rebase();
    stopping_ = false;
    thread_ = std::thread([this] { threadMain(); });
}

void ProgramLoop::stop() {
    std::lock_guard c(controlMu_);
    if (!thread_.joinable()) return;
    stopping_ = true;
    thread_.join();
}

void ProgramLoop::runOffline(int64_t ticks) {
    std::lock_guard c(controlMu_);
    assert(!thread_.joinable() && "runOffline while the real-time loop runs");
    if (thread_.joinable()) return;
    prepare(false);
    for (int64_t i = 0; i < ticks; ++i) runTick(false);
}

void ProgramLoop::prepare(bool realtime) {
    // Offline runs continue on their clock; each real-time start begins on a fresh one.
    if (clock_ && !realtime && !clockRealtime_) return;
    // A new clock: every input reopens on it at the next tick.
    clock_ = realtime ? SessionClock::realtime() : SessionClock::manual();
    clockRealtime_ = realtime;
    offset_ = realtime ? INT64_MIN : 0;
    if (realtime) origin_ = Steady::now() - toSteady(clock_->now());
    std::lock_guard l(mu_);
    reopen_.fill(true);
}

void ProgramLoop::rebase() {
    // Tick n reads input time tickTime(n + offset_) and is due when the clock reads that (up to
    // 1 ms past still counts as now). Steps of 5 ticks keep samplesInTick(n + offset_) ==
    // samplesInTick(n). Never moves backwards, so the inputs are never asked for earlier times.
    const SessionTime now = clock_->now() - 240;
    const int64_t k = roundUpTo(ceilDiv(now, tickTime(1)) - tick_, 5);
    offset_ = std::max(offset_, k);
}

void ProgramLoop::threadMain() {
    DeadlineTimer timer;
    while (!stopping_) {
        Steady::time_point deadline = origin_ + toSteady(tickTime(tick_ + offset_));
        const auto behind = Steady::now() - deadline;
        if (behind > kStallAfter) {
            rebase();
            deadline = origin_ + toSteady(tickTime(tick_ + offset_));
            std::lock_guard l(mu_);
            ++stats_.stalls;
        } else if (behind > kLateAfter) {
            std::lock_guard l(mu_);
            ++stats_.lateTicks;
        }
        timer.waitUntil(deadline);
        if (stopping_) break;
        runTick(true);
    }
}

void ProgramLoop::runTick(bool realtime) {
    const auto began = Steady::now();
    const int64_t n = tick_;
    const int64_t at = n + offset_;   // the inputs' tick
    if (!realtime) clock_->set(tickTime(at)); // Ruling 1

    // 1. Inputs replaced by setInput (opened now, so offline they start at this tick), then the
    // posted commands.
    std::array<std::optional<InputConfig>, 4> reopen;
    {
        std::lock_guard l(mu_);
        for (size_t i = 0; i < 4; ++i)
            if (reopen_[i]) {
                reopen[i] = configs_[i];
                reopen_[i] = false;
            }
        running_.swap(posted_);
    }
    for (size_t i = 0; i < 4; ++i) {
        if (!reopen[i]) continue;
        auto fresh = std::make_unique<Input>(*reopen[i], geometry_, *clock_, delayMs_);
        {
            std::lock_guard l(mu_);
            inputs_[i].swap(fresh);
        }
        if (fresh) {
            // The old input's decode thread joins elsewhere, so this tick does not wait for it.
            std::erase_if(retiring_, [](const std::future<void>& f) {
                return f.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
            });
            retiring_.push_back(std::async(std::launch::async, [old = std::move(fresh)]() mutable { old.reset(); }));
        }
    }
    {
        // Cleared however the loop is left, so no command can run twice.
        struct Clear {
            std::vector<std::function<void(vt::Switcher&)>>& v;
            ~Clear() { v.clear(); }
        } clear{running_};
        for (auto& f : running_) guarded("a posted command", [&] { f(sw_); });
        if (!running_.empty()) nameStale_ = true;
    }

    // 2-3. Every input, on air or not (offline, a blocking read of each before the clock moves
    // on keeps file anchors independent of decode speed).
    const Wait wait = realtime ? Wait::No : Wait::Block;
    const int64_t delay = realtime ? delayMs_ : 0;
    const SessionTime t = tickTime(at) - delay * 240;
    const int64_t first = samplesBefore(at) - delay * 48;
    const size_t frames = size_t(samplesInTick(n));
    vt::SourceFrames src;
    std::array<std::span<const float>, 4> sound;
    for (size_t i = 0; i < 4; ++i) {
        const std::span<float> a(audio_[i].data(), frames * 2);
        src.inputs[i] = inputs_[i]->videoAt(t, frames_[i], wait) ? &frames_[i] : nullptr;
        inputs_[i]->audioAt(first, a, wait);
        sound[i] = a;
    }

    // 4-6. Program picture and sound.
    sw_.render(src, program_);
    std::shared_ptr<const vt::Sound> effectSound = sw_.pendingSound(); // the mixer keeps it while it plays
    const std::span<float> mix(mix_.data(), frames * 2);
    mixer_.process(MixState::of(sw_), sound, std::move(effectSound), mix);

    // 7. Sinks.
    {
        std::lock_guard l(sinkMu_);
        for (ProgramSink* s : sinks_) guarded("a sink", [&] { s->onProgram(n, program_, mix); });
    }
    {
        std::lock_guard l(monitorMu_);
        if (!monitors_.empty()) {
            sw_.renderSource(sw_.selected(vt::Bus::Preview), src, preview_);
            for (MonitorSink* m : monitors_) guarded("a monitor", [&] { m->onPictures(n, program_, preview_); });
        }
    }

    // 8.
    sw_.tick();
    ++tick_;
    publish();
    const double ms = std::chrono::duration<double, std::milli>(Steady::now() - began).count();
    std::lock_guard l(mu_);
    ++stats_.ticks;
    stats_.maxTickMs = std::max(stats_.maxTickMs, ms);
}

} // namespace vt::media
