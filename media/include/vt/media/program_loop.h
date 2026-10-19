#pragma once
// The program loop: owns the Switcher, the four inputs and the audio mixer, and runs one tick
// per 59.94 fps frame, handing each tick's Program picture and audio to the sinks. It runs in
// real time on its own thread (start/stop) or offline on the caller's thread as fast as the
// inputs decode (runOffline).
//
// Per tick n: (1) inputs replaced by setInput are opened, then the posted commands run;
// (2) t = tickTime(n) - delay; (3) every input is read: videoAt(t) and audioAt(samplesBefore(n) -
// delay in samples), all four whether on air or not; (4) Switcher::render; (5) pendingSound();
// (6) AudioMixer::process; (7) the sinks, then the monitors (the Preview picture is rendered only
// while one is attached); (8) Switcher::tick, then the Switcher snapshot is published.
//
// Clocks: the inputs open on the first tick that runs, on a clock for the mode it runs in: real
// time shares one SessionClock::realtime() between all four (inputs are read with Wait::No,
// `presentationDelayMs` in the past); offline uses a manual clock set to tickTime(n) at the start
// of tick n, so an input opened at tick n starts there (Ruling 1), and reads with Wait::Block and
// no delay, so the result does not depend on how fast files open or decode. Running in the other
// mode than the last run, and every start(), reopens all inputs on a new clock (so a stopped
// run's paused files and live backlogs are not replayed); consecutive runOffline calls continue
// on the same clock. Tick numbers count on across runs.
//
// Real-time pacing: tick n is due at absolute deadline t0 + tickTime(n) (steady clock, waited on
// a high-resolution waitable timer). A tick that starts more than 1 ms after its deadline runs at
// once and counts in lateTicks; ticks are never skipped. More than 1 s behind, the loop re-bases
// t0 so the tick is due now (or within 5 ticks: t0 moves in steps of 5 ticks, which keeps every
// tick's sample count equal to samplesInTick(tick)) and counts a stall; the inputs are then
// sampled at the current time again, so their pictures and audio jump forward.
//
// Threading: post, postChecked, setInput, addSink, removeSink, addMonitor, removeMonitor,
// switcherState, mixer()'s setters and meters(), inputStatus and stats may be called from any thread. Only the loop thread (or runOffline's caller) touches the
// Switcher. An input replaced by setInput is destroyed on a separate thread, so the loop never
// waits for its decode thread to join. Sinks are called on the loop thread, synchronously: a slow
// sink slows the loop (offline that is intended). removeSink waits for a running sink call to
// finish: once it returns, that sink is not called again; addMonitor and removeMonitor work the
// same way for monitors. switcherState() copies the latest snapshot under a short lock that the
// loop holds only to publish the next one.
//
// From the loop thread itself, that is inside onProgram or a posted command:
// - post, setInput, mixer()'s setters and meters(), inputStatus and stats are fine;
// - addSink and removeSink are fine in a posted command but deadlock inside onProgram, as do
//   addMonitor and removeMonitor inside onPictures (the sinks' and the monitors' locks are held
//   while they run); switcherState is fine anywhere;
// - start, stop and runOffline must never be called (stop would join its own thread, runOffline
//   and an offline start would wait for the run they are called from).
//
// Exceptions: a posted command or a sink call that throws is logged (vt::logWarning) and skipped;
// the tick and the run go on, and the command is not run again.
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "vt/media/geometry.h"
#include "vt/media/input.h"
#include "vt/media/mixer.h"
#include "vt/media/monitor.h"
#include "vt/media/time.h"
#include "vt/raster.h"
#include "vt/switcher.h"

namespace vt::media {

class ProgramSink {
public:
    virtual ~ProgramSink() = default;
    // Called on the loop thread once per tick, in tick order. `program` is valid only during
    // the call (copy what you keep); stereo has samplesInTick(tick) frames at 48 kHz.
    virtual void onProgram(int64_t tick, const vt::Frame& program, std::span<const float> stereo) = 0;
};

struct LoopStats { int64_t ticks = 0, lateTicks = 0, stalls = 0; double maxTickMs = 0; };

struct ProgramConfig {
    std::array<InputConfig, 4> inputs;
    MediaGeometry geometry;
    int presentationDelayMs = 100;   // real time only: inputs are sampled this far in the past
};

class ProgramLoop {
public:
    explicit ProgramLoop(ProgramConfig);
    ~ProgramLoop();                                   // stop()
    ProgramLoop(const ProgramLoop&) = delete;
    ProgramLoop& operator=(const ProgramLoop&) = delete;

    void post(std::function<void(vt::Switcher&)>);    // runs once, at the start of the next tick
    // Like post, for a call that can be refused (Switcher::setEffect): when `f` returns false, `why`
    // becomes SwitcherSnapshot::lastRefusal (an empty `why` reads "refused"), in that tick's
    // snapshot; when it returns true, lastRefusal is cleared. A plain post never touches it.
    void postChecked(std::function<bool(vt::Switcher&, std::string& why)>);
    void setInput(int index, const InputConfig&);     // replaces input index (0..3) at the next tick
    AudioMixer& mixer() { return mixer_; }            // its setters are thread-safe
    void addSink(ProgramSink*);  void removeSink(ProgramSink*);   // thread-safe
    void addMonitor(MonitorSink*);  void removeMonitor(MonitorSink*);   // thread-safe
    // The Switcher after the latest finished tick (its defaults before the first one), published
    // every tick. Thread-safe.
    SwitcherSnapshot switcherState() const;
    void start();                // real time on its own thread, inputs reopened; does nothing if running
    void stop();                 // joins the thread; does nothing if not running
    // On the calling thread, inputs read with Wait::Block, delay 0. Not while start()ed (asserts;
    // returns at once in release builds).
    void runOffline(int64_t ticks);
    InputStatus inputStatus(int index) const;         // Idle before the inputs first open
    LoopStats stats() const;

private:
    void prepare(bool realtime);     // the clock and inputs for a run in this mode
    void rebase();                   // real time: offset_ so the next tick is due now (or soon)
    void runTick(bool realtime);
    void publish();                  // work_ from the Switcher, then the snapshot switcherState() returns
    void threadMain();

    const MediaGeometry geometry_;
    const int delayMs_;

    // Loop thread (or runOffline's caller) only.
    vt::Switcher sw_;
    std::array<std::unique_ptr<Input>, 4> inputs_;   // swapped under mu_ (inputStatus reads them)
    std::array<vt::Frame, 4> frames_;                // each input's latest picture
    vt::Frame program_, preview_;                    // preview_: the Preview source, for the monitors
    SwitcherSnapshot work_;                          // the snapshot being built
    bool nameStale_ = true;                          // the effect may have changed since work_.effectName
    std::array<std::vector<float>, 4> audio_;        // one tick of each input, 801 stereo frames
    std::vector<float> mix_;                         // one tick of Program audio
    std::vector<std::function<void(vt::Switcher&)>> running_;  // the commands being run
    std::vector<std::future<void>> retiring_;        // replaced inputs being destroyed
    std::optional<SessionClock> clock_;
    bool clockRealtime_ = false;
    std::chrono::steady_clock::time_point origin_;   // real time: when clock_ read 0
    int64_t tick_ = 0;
    int64_t offset_ = 0;  // input time of tick n is tickTime(n + offset_); a multiple of 5

    AudioMixer mixer_;

    mutable std::mutex mu_;  // the members below
    std::array<InputConfig, 4> configs_;
    std::array<bool, 4> reopen_{};                   // configs_[i] waits to be opened
    std::vector<std::function<void(vt::Switcher&)>> posted_;
    LoopStats stats_;

    std::mutex sinkMu_;      // held while the sinks run
    std::vector<ProgramSink*> sinks_;
    std::mutex monitorMu_;   // held while the monitors run
    std::vector<MonitorSink*> monitors_;

    mutable std::mutex snapMu_;   // held only to copy a snapshot
    SwitcherSnapshot snapshot_;   // the published one

    std::mutex controlMu_;   // start, stop and runOffline
    std::thread thread_;
    std::atomic<bool> stopping_{false};
};

} // namespace vt::media
