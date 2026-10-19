#pragma once
// The recorder: a ProgramSink that writes the Program picture (the recordRect crop of the raster,
// 720x480, SAR 10:11, 59.94 fps) and its mixed audio (48 kHz stereo) to a file. Encoding runs on
// the recorder's own thread, fed from a queue of copies, so the loop only pays for the copy.
//
// Timing: video pts = ticks since the first recorded tick, audio pts = samples since then.
// Colour: H.264 (yuv420p) and ProRes 422 HQ (yuv422p10le) are converted with swscale from the
// raster's full-range RGB to limited-range SMPTE 170M and tagged so; FFV1 stores the pixels
// exactly (bgr0). H.264 audio is AAC, FFV1 and ProRes carry pcm_s16le.
//
// Queue: at most 120 frames wait for the encoder. Beyond that a new frame is dropped and counted
// (its audio is still queued: audio is never dropped), unless `blockWhenFull` makes onProgram wait
// for room instead (offline rendering). Any error (open, write, disk full) lands in
// status().error, after which onProgram ignores its input; nothing is ever thrown to the loop.
// stop() after an error still writes the trailer when it can, so an MKV stays playable.
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>

#include "vt/media/geometry.h"
#include "vt/media/program_loop.h"

namespace vt::media {

enum class RecordFormat { H264Mkv, H264Mp4, Ffv1Mkv, ProResMov };

struct RecorderSettings {
    std::filesystem::path path;
    RecordFormat format = RecordFormat::H264Mkv;
    int crf = 16;                       // H.264 (libx264, preset veryfast, yuv420p, High)
    int audioKbps = 192;                // AAC for H.264 formats; FFV1/ProRes use pcm_s16le
    MediaGeometry geometry;
    bool blockWhenFull = false;         // true: onProgram waits for queue room instead of dropping
#ifdef VT_MEDIA_TEST_SEAMS
    int64_t failAfterBytesForTest = 0;  // > 0: the muxer "fails" once this many packet bytes are written
#endif
};

struct RecorderStatus {
    bool recording = false;
    std::string error;
    int64_t framesWritten = 0, framesDropped = 0;
};

class Recorder : public ProgramSink {
public:
    Recorder();
    ~Recorder() override;                                       // stop()
    Recorder(const Recorder&) = delete;
    Recorder& operator=(const Recorder&) = delete;

    // Opens the file and the encoders and starts the encoder thread. On failure returns false
    // with the reason in *error (when given) and in status().error. While a recording runs it
    // returns false ("already recording"), unless that recording failed (status().error set):
    // then it is finished first, as by stop(), and the new one starts.
    bool start(const RecorderSettings&, std::string* error);
    // Drains the queue, flushes the encoders, writes the trailer and closes the file. It blocks
    // until everything queued is encoded (up to 120 frames: about a second for H.264, more on a
    // slow machine); a producer blocked in onProgram returns at once. Safe to call at any time,
    // from any thread but the loop thread's onProgram, also from several at once (the others
    // return once it has stopped). A no-op when not started.
    void stop();
    RecorderStatus status() const;
    // The loop thread, once per tick. The picture and audio are copied before it returns.
    void onProgram(int64_t tick, const vt::Frame&, std::span<const float> stereo) override;

#ifdef VT_MEDIA_TEST_SEAMS
    void setEncoderPausedForTest(bool paused);                  // the encoder thread takes no entries while true
#endif

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace vt::media
