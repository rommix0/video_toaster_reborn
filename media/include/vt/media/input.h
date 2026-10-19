#pragma once
// A video/audio source on its own decode thread: pictures converted into the engine raster and
// sound into 48 kHz stereo, both stamped in session time so the program loop can sample them
// tick by tick. Files open now; devices and the desktop arrive with the capture work.
#include <cstdint>
#include <map>
#include <memory>
#include <span>
#include <string>

#include "vt/media/geometry.h"
#include "vt/media/time.h"
#include "vt/raster.h"

namespace vt::media {

enum class InputKind { None, File, Device, Screen };

struct InputConfig {
    InputKind kind = InputKind::None;
    std::string path;            // File
    bool loop = true;            // File: restart at the end (false: hold the last frame, then silence)
    std::string videoDevice;     // Device (Task 9)
    std::string audioDevice;     // Device or Screen (Task 9), "" = none
    std::string window;          // Screen (Task 9): "" = desktop, else window title
    std::map<std::string, std::string> options;  // extra avformat options (video_size, framerate...)
    int retryMs = 2000;          // reopen interval after a failure
};

struct InputStatus {
    enum class State { Idle, Opening, Running, Ended, Error } state = State::Idle;
    std::string message;         // why, for Error; the input's description otherwise
    int width = 0, height = 0;   // source picture
    bool hasAudio = false;
    uint64_t framesDecoded = 0, framesLate = 0;
    uint64_t decodeErrors = 0;   // damaged packets or frames that were skipped
};

enum class Wait { No, Block };

class Input {
public:
    // A File input starts opening at once, on its own thread. Its first frame is due at
    // clock.now() as measured when the file opens; the stream's timestamps count from there.
    Input(const InputConfig&, const MediaGeometry&, SessionClock clock);
    ~Input();                                    // stops and joins the thread
    Input(const Input&) = delete;
    Input& operator=(const Input&) = delete;

    // The latest picture whose session time <= t, copied into out (skips the copy and returns
    // true if out already holds that picture for this input). False = no picture: caller shows black.
    // Times within 0.5 ms after t also count, to absorb container timestamp rounding (Matroska
    // stores whole milliseconds). Give each input its own `out`: the skip trusts that nothing
    // else wrote into it since this input filled it.
    // Wait::Block returns once the thread has decoded past t, or the input ended or failed;
    // Wait::No returns what is there now.
    bool videoAt(SessionTime t, vt::Frame& out, Wait);
    // Samples [first, first + stereo.size()/2) of this input; silence where there are none.
    // Wait::Block waits until the thread has written past the span, or finished the pass that
    // covers it without writing there (or the input ended or failed).
    void audioAt(int64_t firstSample, std::span<float> stereo, Wait);
    InputStatus status() const;

    struct Shared; // the state the decode thread works on (src/input_internal.h)

private:
    std::unique_ptr<Shared> s_;
    // What videoAt last copied, to skip copying the same picture again.
    const vt::Frame* lastOut_ = nullptr;
    const vt::Rgba8* lastOutPx_ = nullptr;
    uint64_t lastOutId_ = 0;
};

} // namespace vt::media
