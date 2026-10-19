#include "vt/media/input.h"

#include <algorithm>
#include <chrono>

#include "input_internal.h"

namespace vt::media {
namespace {

using State = InputStatus::State;

constexpr size_t kPoolMax = 4; // spare rasters kept for reuse (1.48 MB each)

// True when `out` still holds `pic`, checked on a sparse grid of pixels. The caller's Frame is
// identified by address, which a new Frame can reuse; the sample catches that case cheaply
// (a fresh Frame is transparent black, converted pictures are opaque).
bool stillHolds(const vt::Frame& out, const vt::Frame& pic) {
    if (out.px.size() != pic.px.size()) return false;
    for (size_t i = 0; i < pic.px.size(); i += 9973)
        if (!(out.px[i] == pic.px[i])) return false;
    return true;
}

} // namespace

// ---- decode-thread side ----

bool Input::Shared::waitForRoom() {
    std::unique_lock l(mu);
    roomCv.wait(l, [&] {
        // A blocked reader wants something not decoded yet: keep going whatever is queued.
        if (stopping || waiters > 0) return true;
        const auto ahead = std::count_if(queue.begin(), queue.end(),
                                         [&](const Picture& p) { return p.time > reader + kPtsSlack; });
        return ahead < kVideoAhead && (!hasAudio || audioEnd - floorDiv(reader, 5) < kAudioAhead);
    });
    return !stopping;
}

bool Input::Shared::sleepFor(int ms) {
    std::unique_lock l(mu);
    roomCv.wait_for(l, std::chrono::milliseconds(ms), [&] { return stopping.load(); });
    return !stopping;
}

vt::Frame Input::Shared::takeBuffer() {
    std::lock_guard l(mu);
    if (pool.empty()) return {};
    vt::Frame f = std::move(pool.back());
    pool.pop_back();
    return f;
}

void Input::Shared::publishPicture(SessionTime t, vt::Frame&& f) {
    {
        std::lock_guard l(mu);
        auto recycle = [&](Picture& p) {
            if (pool.size() < kPoolMax) pool.push_back(std::move(p.frame));
        };
        while (!queue.empty() && queue.back().time >= t) {
            recycle(queue.back());
            queue.pop_back();
        }
        if (t <= videoShown) ++status.framesLate; // the reader already showed that moment without it
        if (queue.size() >= kVideoMax) {
            recycle(queue.front());
            queue.pop_front();
            ++status.framesLate;
        }
        queue.push_back({t, nextId++, std::move(f)});
        videoThrough = t;
        ++status.framesDecoded;
        advanceReader(reader);
    }
    dataCv.notify_all();
}

void Input::Shared::publishAudio(int64_t firstSample, std::span<const float> stereo) {
    ring.write(firstSample, stereo);
    {
        std::lock_guard l(mu);
        audioEnd = ring.endSample();
    }
    dataCv.notify_all();
}

void Input::Shared::passComplete(SessionTime end) {
    {
        std::lock_guard l(mu);
        videoThrough = std::max(videoThrough, end - 1);
        audioThrough = std::max(audioThrough, floorDiv(end, 5));
    }
    dataCv.notify_all();
}

void Input::Shared::setOpening() {
    int64_t silenceFrom = 0;
    {
        std::lock_guard l(mu);
        status.state = State::Opening;
        status.message = name;
        status.width = status.height = 0;
        status.hasAudio = hasVideo = hasAudio = false;
        silenceFrom = dropAhead();
    }
    ring.silenceFrom(silenceFrom);
}

void Input::Shared::setRunning(int width, int height, bool video, bool audio) {
    {
        std::lock_guard l(mu);
        status.state = State::Running;
        status.message = name;
        status.width = width;
        status.height = height;
        status.hasAudio = hasAudio = audio;
        hasVideo = video;
        // A reader waits for this run's pictures; without video there are none to wait for.
        videoThrough = video ? INT64_MIN : INT64_MAX;
        audioThrough = INT64_MIN;
    }
    dataCv.notify_all();
}

void Input::Shared::setPictureSize(int width, int height) {
    std::lock_guard l(mu);
    status.width = width;
    status.height = height;
}

void Input::Shared::setState(State st, std::string message) {
    int64_t silenceFrom = INT64_MAX;
    {
        std::lock_guard l(mu);
        status.state = st;
        status.message = std::move(message);
        if (st == State::Error) silenceFrom = dropAhead();
    }
    if (silenceFrom != INT64_MAX) ring.silenceFrom(silenceFrom);
    dataCv.notify_all();
}

void Input::Shared::countDecodeError() {
    std::lock_guard l(mu);
    ++status.decodeErrors;
}

// ---- shared helpers (under mu) ----

void Input::Shared::advanceReader(SessionTime t) {
    reader = std::max(reader, t);
    while (queue.size() >= 2 && queue[1].time <= reader + kPtsSlack) {
        if (pool.size() < kPoolMax) pool.push_back(std::move(queue.front().frame));
        queue.pop_front();
    }
}

bool Input::Shared::busy() const { return status.state == State::Opening || status.state == State::Running; }

int64_t Input::Shared::dropAhead() {
    for (Picture& p : queue)
        if (pool.size() < kPoolMax) pool.push_back(std::move(p.frame));
    queue.clear();
    videoThrough = INT64_MIN;
    videoShown = INT64_MIN;
    return floorDiv(reader, 5);
}

// ---- Input ----

Input::Input(const InputConfig& cfg, const MediaGeometry& geo, SessionClock clock, int presentationDelayMs)
    : s_(std::make_unique<Shared>(cfg, geo, std::move(clock), presentationDelayMs)) {
    switch (cfg.kind) {
    case InputKind::None:
        break; // Idle: no picture, silence
    case InputKind::Device:
    case InputKind::Screen:
        if (std::string why = liveConfigError(cfg); !why.empty()) {
            s_->status.state = State::Error; // nothing to retry: the config itself is unusable
            s_->status.message = std::move(why);
            break;
        }
        [[fallthrough]];
    case InputKind::File:
        s_->status.state = State::Opening;
        s_->status.message = s_->name;
        s_->thread = std::thread([s = s_.get()] { runInput(*s); });
        break;
    }
}

Input::~Input() {
    if (!s_->thread.joinable()) return;
    {
        std::lock_guard l(s_->mu);
        s_->stopping = true;
    }
    s_->roomCv.notify_all();
    s_->dataCv.notify_all();
    s_->thread.join();
}

bool Input::videoAt(SessionTime t, vt::Frame& out, Wait wait) {
    Shared& s = *s_;
    const SessionTime due = t + kPtsSlack;
    std::unique_lock l(s.mu);
    s.advanceReader(t);
    s.roomCv.notify_all();
    if (wait == Wait::Block) {
        const auto ready = [&] {
            return s.stopping || !s.busy() || (s.status.state == State::Running && s.videoThrough >= due);
        };
        ++s.waiters;
        if (s.live) s.dataCv.wait_for(l, kLiveBlockWait, ready);
        else s.dataCv.wait(l, ready);
        --s.waiters;
        s.advanceReader(t);
    }
    s.videoShown = std::max(s.videoShown, due);

    const Picture* pic = nullptr;
    for (const Picture& p : s.queue) {
        if (p.time > due) break;
        pic = &p;
    }
    if (!pic) return false;
    if (lastOut_ == &out && lastOutPx_ == out.px.data() && lastOutId_ == pic->id && stillHolds(out, pic->frame))
        return true;
    out.px = pic->frame.px; // same size: copies without reallocating
    lastOut_ = &out;
    lastOutPx_ = out.px.data();
    lastOutId_ = pic->id;
    return true;
}

void Input::audioAt(int64_t firstSample, std::span<float> stereo, Wait wait) {
    Shared& s = *s_;
    const int64_t end = firstSample + int64_t(stereo.size() / 2);
    {
        std::unique_lock l(s.mu);
        s.advanceReader(firstSample * 5);
        s.roomCv.notify_all();
        if (wait == Wait::Block) {
            const auto ready = [&] {
                return s.stopping || !s.busy() ||
                       (s.status.state == State::Running && (!s.hasAudio || std::max(s.audioEnd, s.audioThrough) >= end));
            };
            ++s.waiters;
            if (s.live) s.dataCv.wait_for(l, kLiveBlockWait, ready);
            else s.dataCv.wait(l, ready);
            --s.waiters;
        }
    }
    s.ring.read(firstSample, stereo);
}

InputStatus Input::status() const {
    std::lock_guard l(s_->mu);
    return s_->status;
}

} // namespace vt::media
