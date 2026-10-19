#pragma once
// What an Input shares with its decode thread. input.cpp owns it and serves the readers;
// input_demux.cpp runs the thread (open, demux, decode, convert, publish).
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "vt/media/audio_convert.h"
#include "vt/media/input.h"

namespace vt::media {

inline constexpr int kVideoAhead = 16;          // converted pictures kept ahead of the reader
inline constexpr size_t kVideoMax = 64;         // queue cap while a blocked reader forces progress, and for live sources
inline constexpr int64_t kAudioAhead = 48000;   // 1 s of audio kept ahead of the reader
inline constexpr SessionTime kPtsSlack = 120;   // 0.5 ms of container rounding (Ruling 6)
// How long a Wait::Block read of a live input waits (about 2 ticks): a live source never ends,
// so the reader takes what has arrived by then.
inline constexpr std::chrono::milliseconds kLiveBlockWait{35};

// Floor division for possibly negative times.
constexpr int64_t floorDiv(int64_t a, int64_t b) { return a / b - (a % b != 0 && (a < 0) != (b < 0)); }

struct Picture {
    SessionTime time = 0;
    uint64_t id = 0;
    vt::Frame frame;
};

// What an input shows in its status and log lines: the file path, the dshow input string, or
// the screen being captured.
std::string describeInput(const InputConfig&);
// Why a Device or Screen config cannot be opened at all ("" if it can): no device named, or a
// name dshow's input string cannot carry.
std::string liveConfigError(const InputConfig&);

struct Input::Shared {
    Shared(const InputConfig& c, const MediaGeometry& g, SessionClock k)
        : cfg(c), name(describeInput(c)), live(c.kind == InputKind::Device || c.kind == InputKind::Screen),
          geo(g), clock(std::move(k)), reader(clock.now()) {}

    const InputConfig cfg;
    const std::string name;
    const bool live;              // a capture source: never ends, never waits for the reader
    const MediaGeometry geo;
    SessionClock clock;
    AudioRing ring;               // written by the thread, read by audioAt (its own lock)
    std::atomic<bool> stopping{false};
    std::thread thread;

    // ---- decode-thread side (each takes mu) ----
    // Waits while the look-ahead is full; false once the input is stopping.
    bool waitForRoom();
    // Waits `ms` or until stopping; false once the input is stopping.
    bool sleepFor(int ms);
    // A raster for the next picture (recycled from consumed ones).
    vt::Frame takeBuffer();
    // Queues a converted picture due at `t`. Queued pictures at or after t are dropped first
    // (a restarted timeline supersedes them); at kVideoMax the oldest is dropped (counted late),
    // which is all the flow control a live source gets.
    void publishPicture(SessionTime t, vt::Frame&& f);
    // Writes audio at `firstSample` into the ring and wakes readers.
    void publishAudio(int64_t firstSample, std::span<const float> stereo);
    // End of a pass: nothing more, picture or sound, can arrive before `end` (the next pass
    // starts there), so readers waiting on the gap, or on a stream that yielded nothing, return.
    void passComplete(SessionTime end);
    void setOpening();
    void setRunning(int width, int height, bool hasVideo, bool hasAudio);
    // The source picture size, as decoded frames show it (a capture may only tell then).
    void setPictureSize(int width, int height);
    void setState(InputStatus::State, std::string message);
    void countDecodeError();

    // ---- shared state, guarded by mu ----
    mutable std::mutex mu;
    std::condition_variable dataCv;   // thread -> readers: new data or a state change
    std::condition_variable roomCv;   // readers -> thread: the reader moved on, or stop
    InputStatus status;
    std::deque<Picture> queue;        // ascending time; front is the picture shown now
    std::vector<vt::Frame> pool;      // spare rasters
    uint64_t nextId = 1;
    bool hasVideo = false, hasAudio = false;
    SessionTime videoThrough = INT64_MIN; // every picture up to here has been queued
    int64_t audioEnd = 0;                 // mirrors ring.endSample()
    int64_t audioThrough = INT64_MIN;     // no more audio will be written before this sample
    SessionTime reader;                   // furthest time either reader asked for
    SessionTime videoShown = INT64_MIN;   // t + slack of the last finished videoAt
    int waiters = 0;                      // readers blocked in Wait::Block

    // Under mu: moves the reader to t and drops pictures it can no longer be shown.
    void advanceReader(SessionTime t);
    // Under mu: true while the decode thread may not be producing what a blocked reader wants.
    bool busy() const;
};

// The decode thread's body (input_demux.cpp).
void runInput(Input::Shared& s);

} // namespace vt::media
