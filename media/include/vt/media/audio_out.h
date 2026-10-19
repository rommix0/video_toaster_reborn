#pragma once
// Live audio output: the program loop's 48 kHz stereo mix played on a Windows audio device
// (WASAPI shared mode, event driven). The loop thread (onProgram) and the device's clock are
// independent, so they meet in a lock-free FIFO, and the render thread resamples by a tiny
// ratio that keeps the FIFO near its target level (DriftController).
//
// Render thread, per device callback: while the FIFO has not yet reached its target level (at
// start, and again after an underrun) the device gets silence and the FIFO is left to fill; once
// it has, the FIFO is cut to the target (so audio the loop pushed in a burst after a stall does
// not stay as extra delay, nor does the overshoot of the priming); then each callback pulls what the device wants, through swresample into the device's mix format
// (any rate, channel count and 16/32 bit integer or float sample format), with
// swr_set_compensation applying DriftController's ratio. A FIFO that runs dry in the middle of a
// callback plays silence for the rest of it (an underrun) and primes again; a level more than
// 20 ms above target gets the same cut. onProgram on a full
// FIFO (one second) makes the oldest audio go (an overrun). A device that goes away (unplugged,
// disabled, format changed) ends the thread with status().error set; stop() still works.
//
// Threading: start, stop and status may be called from any thread (start and stop one at a
// time); onProgram is for the loop thread: it does not block, allocate or take a lock.
#include <atomic>
#include <cstdint>
#include <future>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "vt/media/program_loop.h"

namespace vt::media {

struct OutputDevice { std::string name, id; bool isDefault = false; };

// The active playback devices (the default one is flagged). Empty when there are none or listing
// fails (the reason goes to *error, which is cleared on success); never throws.
std::vector<OutputDevice> listOutputDevices(std::string* error = nullptr);

struct AudioOutputSettings {
    std::string deviceId;   // OutputDevice::id; "" = the default device
    int bufferMs = 60;      // the device buffer; the FIFO target level follows it (at least ~35 ms)
};

// Keeps a FIFO near a target level by a resample ratio close to 1: the number of input frames to
// consume per nominal one (above 1 when the FIFO is above target, so it drains; below 1 when
// below). Call once per device callback with the FIFO's frame count. The level is smoothed
// (callbacks arrive in bursts and the loop pushes a tick at a time) and the correction is
// proportional to its distance from the target, clamped to kMaxDeviation (so any pitch change
// stays inaudible); with clocks a few hundred ppm apart it settles within a few ms of the target.
class DriftController {
public:
    static constexpr double kMaxDeviation = 0.002;
    explicit DriftController(int targetFrames);
    double ratio(int fifoFrames);          // in [1 - kMaxDeviation, 1 + kMaxDeviation]
private:
    double target_;
    double level_ = 0;                     // the smoothed FIFO level
    bool seeded_ = false;
};

// What the render thread does each callback with the FIFO level: wait for the target level (at
// start and after an underrun), cut the FIFO to the target when it primes or is more than
// kTrimMargin above it, and otherwise play with the DriftController's ratio.
class FifoPacer {
public:
    static constexpr int kTrimMargin = 960;   // 20 ms of 48 kHz frames
    struct Step {
        bool play = false;     // false: the device gets silence and the FIFO keeps filling
        int keepFrames = -1;   // >= 0: drop the oldest audio first, so this many frames remain
        double ratio = 1.0;    // when playing: input frames per nominal one, after any cut
    };
    explicit FifoPacer(int targetFrames);
    Step step(int fifoFrames);   // once per callback, with the level before it
    void ranDry() { primed_ = false; }   // the FIFO could not fill a callback
private:
    int target_;
    DriftController drift_;
    bool primed_ = false;
};

class AudioOutput : public ProgramSink {
public:
    AudioOutput();
    ~AudioOutput() override;               // stop()
    AudioOutput(const AudioOutput&) = delete;
    AudioOutput& operator=(const AudioOutput&) = delete;

    // Opens the device and starts the render thread; false with the reason in *error (also in
    // status().error) when it cannot, or when already running.
    bool start(const AudioOutputSettings&, std::string* error);
    void stop();                           // joins the render thread; does nothing if not running
    // Queues one tick of Program audio for the device. Ignored while not running.
    void onProgram(int64_t tick, const vt::Frame&, std::span<const float> stereo) override;

    struct Status {
        bool running = false;
        std::string error;                 // why it failed to start or stopped by itself; "" if fine
        int64_t underruns = 0, overruns = 0;
        double driftPpm = 0;               // the correction in use: (ratio - 1) * 1e6
    };
    Status status() const;

private:
    void renderMain(const AudioOutputSettings&, std::promise<std::string>&);   // the render thread; sets `ready` once
    void setError(const std::string&);

    // The FIFO: power-of-two frame ring, one producer (onProgram) and one consumer (the render
    // thread). w_ and r_ count frames since start; a full FIFO is trimmed by the consumer.
    static constexpr int64_t kFifoFrames = 48000;        // what the FIFO holds
    static constexpr int64_t kRingFrames = 131072;       // physical size: room to overwrite the trimmed part
    std::vector<float> ring_;
    std::atomic<int64_t> w_{0}, r_{0};

    std::atomic<bool> running_{false};
    std::atomic<int64_t> underruns_{0}, overruns_{0};
    std::atomic<double> driftPpm_{0};
    mutable std::mutex mu_;                // error_
    std::string error_;
    std::thread thread_;
    void* stopEvent_ = nullptr;            // HANDLE, signalled by stop()
};

} // namespace vt::media
