# Plan 2 — Video and Audio I/O (FFmpeg) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Feed the Plan 1 engine with four live inputs (files, DirectShow devices, screen capture) and their audio, run the Switcher at 59.94 fps against a real clock, mix the audio so it follows the video, and record Program with its audio to a file. No Qt yet.

**Architecture:** A new static library `vt_media` (in `media/`) depends on `vt_engine` and FFmpeg, and has no Qt. Each input decodes on its own thread and converts **once**:
- video goes to the engine raster (768×482 RGBA, placed and aspect-fitted, then never scaled again);
- audio goes to 48 kHz stereo float, stamped on a shared session clock.

A `ProgramLoop` owns the `vt::Switcher`. On every tick it gathers the input pictures for that tick's presentation time, renders, mixes that tick's audio and hands both to sinks such as the `Recorder`. It can run against the wall clock (real time) or as fast as possible with blocking reads (offline), which makes every test deterministic.

**Tech Stack:**
- C++20, MSVC 14.44 (VS2022 BuildTools), CMake 3.31 + Ninja, Catch2 v3.7.1 (already in `third_party/`).
- FFmpeg 9.0.2 shared build at `../ffmpeg-9.0.2-full_build-shared`: libavcodec 63, libavformat 63, libavdevice 63, libavutil 61, libswscale 10, libswresample 7. This build was verified with MSVC: linking, an FFV1 encode, prores_ks and dshow present.

**Spec:** `docs/2026-10-17-switcher-design.md` (§2, §3 item 1, §5.10, §5.11, §6, §8). Plan 1 (`docs/plans/2026-10-18-plan1-engine-core.md`) is merged on `main`. Its ledger, `.superpowers/sdd/2026-10-18-plan1-engine-core/progress.md`, holds Rulings 1–33, which still bind the engine.

## Global Constraints

**Libraries and build**
- `vt_engine` stays free of FFmpeg and Qt. `vt_media` may use FFmpeg and the engine, never Qt.
- FFmpeg comes only from `VT_FFMPEG_DIR` (CMake cache path, default `${CMAKE_SOURCE_DIR}/../ffmpeg-9.0.2-full_build-shared`). Its DLLs are never committed to git. Every executable that links `vt_media` gets **all** `bin/*.dll` copied next to it, because avdevice-63.dll needs avfilter-12.dll.
- The FFmpeg 9 headers in `include/` are the API reference. APIs removed in FFmpeg 7–9 do not exist: no `avcodec_close`, no `av_init_packet`, no `channel_layout` bitmask. Use `AVChannelLayout`, `swr_alloc_set_opts2`, `sws_scale_frame`, `avcodec_send_*`/`receive_*`.
- No OBS code is copied. `C:\Users\abart\Desktop\obs-studio-master` may be read for ideas only.

**Timing and picture**
- Clock: one Switcher tick = one 59.94 fps frame = 1001/60000 s.
- Session time is an `int64_t` in units of **1/240000 s**: tick *n* = n × 4004, 48 kHz sample *s* = s × 5, a 30000/1001 frame = 8008, a 25 fps frame = 9600.
- Internal audio: **48000 Hz, stereo, float32 interleaved**. Tick *n* owns samples `[samplesBefore(n), samplesBefore(n+1))` with `samplesBefore(n) = floor(n × 4004 / 5)`, which gives 800 or 801 samples per tick.
- Progressive in and out (owner). Interlaced sources are taken as progressive frames: no deinterlacer in v1. Log one warning per input when frames carry `AV_FRAME_FLAG_INTERLACED`.
- Clean video: no blending between source frames. Frame-rate conversion is sample-and-hold.
- `libswscale` scales only on the way in. On the way out it converts colour at the same size and never resamples.
- **Input geometry (my call, adjustable in `MediaGeometry`):** the raster is treated as Rec.601 NTSC pixels with pixel aspect 10:11.
  - A 4:3 source fills `inputRect = {32, 1, 704, 480}`.
  - Other display aspects are fitted inside it, centred with black bars, and never stretched.
- **Recording (my call):** the crop is `recordRect = {24, 1, 720, 480}`, tagged SAR 10:11, so the file is a standard 720×480 NTSC frame that players show at 4:3 without the pixels being resampled. Colour tags are SMPTE 170M, limited range, for YUV formats.

**Engine contract:** call `sw.render(sources, program)` and then `sw.tick()` for every output frame (switcher.h). Only the loop thread touches the Switcher.

**Tests and tooling**
- Tests that need real devices or the desktop are tagged `[live]` and labelled `live` in CTest. The default `tools\build.bat` run excludes them (`-LE live`); `tools\live.bat` runs only them.
- Test media are generated at build time by `ffmpeg.exe` from `VT_FFMPEG_DIR/bin` (lossless FFV1 or PCM in MKV, so pixel values are exact).
- Licensing is deferred by the owner. Note only that this FFmpeg build is GPLv3; do nothing about it.

## Review Focus

1. **An input that fails mid-show** (device unplugged, file missing, corrupt or truncated): that input shows black and silence, Program keeps running, `status()` says why, and devices and missing files are retried every `retryMs`. Tested in Task 5 and Task 9.
2. **Sources at other rates and long runs** (25, 30, 50 and 60 fps, VFR timestamps, 10-minute sessions): each tick shows the latest frame due, with no drift between audio and video and no sample-count error. Tested in Task 2 (schedule over 36000 ticks), Task 5 (25 fps mapping) and Task 7 (delivered sample totals).
3. **Non-4:3 and non-square sources** (1080p 16:9, portrait phone video, SAR 8:9 DV, SAR 0/1 unknown): aspect preserved, black bars, never stretched; a 704×480 SAR 10:11 source is copied pixel for pixel. Tested in Task 2 and Task 3.
4. **The same source on Main and Preview, or a silent source** (BKG, DV1, DV2, an input without audio) in a transition: no level bump or dip, no noise. Tested in Task 6.
5. **Recording that cannot proceed** (unwritable path, disk full, encoder missing): `start()` or `status()` reports the error, Program keeps running, and `stop()` leaves a playable file holding what was written. Tested in Task 8.

---

## File Structure

```
cmake/VtFFmpeg.cmake          imported targets vt::ffmpeg (+ vt_copy_ffmpeg_dlls(target))
media/CMakeLists.txt          vt_media static library
media/include/vt/media/
  ffmpeg.h                    RAII deleters for AV* types, averr(int) -> std::string
  time.h                      SessionTime, tick/sample arithmetic, SessionClock
  geometry.h                  MediaGeometry, fitRect
  video_convert.h             VideoConverter (AVFrame -> vt::Frame)
  audio_convert.h             AudioConverter (any audio -> 48k stereo float), AudioRing
  input.h                     InputConfig, InputStatus, Input
  devices.h                   listDevices (dshow)
  mixer.h                     AudioMixer, MixState, channels, meters
  program_loop.h              ProgramLoop, ProgramSink, LoopStats
  recorder.h                  RecorderSettings, Recorder (a ProgramSink)
media/src/*.cpp               one per header (+ input_demux.cpp for the demux/decode thread)
cli/vt_show.cpp               scripted show -> recording (offline) or live run (real time)
tests/media/CMakeLists.txt    vt_media_tests + fixtures
tests/media/make_fixtures.cmake
tests/media/test_*.cpp        one per media header
tools/live.bat                runs the [live] tests
```

Modify:
- root `CMakeLists.txt`: add `media`, include `cmake/VtFFmpeg.cmake`.
- `cli/CMakeLists.txt`: add `vt_show`.
- `tests/CMakeLists.txt`: add `add_subdirectory(media)`.
- `tools/build.bat`: `ctest --preset msvc -LE live %*`.

---

### Task 1: FFmpeg in the build, `vt_media` skeleton, test fixtures

**Files:**
- Create: `cmake/VtFFmpeg.cmake`
- Create: `media/CMakeLists.txt`, `media/include/vt/media/ffmpeg.h`, `media/src/ffmpeg.cpp`
- Create: `tests/media/CMakeLists.txt`, `tests/media/make_fixtures.cmake`, `tests/media/test_ffmpeg.cpp`, `tools/live.bat`
- Modify: root `CMakeLists.txt`, `tests/CMakeLists.txt`, `tools/build.bat`

**Interfaces:**
- Produces:
  - `vt::ffmpeg`, an INTERFACE target linking imported `avcodec avformat avdevice avutil swscale swresample`. IMPORTED_IMPLIB is `lib/<name>.lib` and IMPORTED_LOCATION is `bin/<name>-<major>.dll`.
  - `vt_copy_ffmpeg_dlls(<target>)`: a POST_BUILD copy of every `bin/*.dll` to `$<TARGET_FILE_DIR:target>`.
  - `media/include/vt/media/ffmpeg.h`:
    - deleters, with `using CodecCtxPtr = std::unique_ptr<AVCodecContext, ...>` and likewise `FormatCtxPtr` (input; close via `avformat_close_input`), `FramePtr`, `PacketPtr`, `SwsPtr`, `SwrPtr`;
    - `std::string averr(int errnum)`;
    - `void initMedia()`: `avdevice_register_all()` once, thread-safe, and FFmpeg logging routed to `vt` log at AV_LOG_WARNING and above.
  - Fixture directory `${CMAKE_BINARY_DIR}/media_fixtures`, passed to the tests as `VT_MEDIA_FIXTURES`. `VT_DATA_ROOT` as in `tests/CMakeLists.txt`.
  - CTest fixture `media_fixtures` (FIXTURES_SETUP); test `vt_media_tests` requires it and runs `vt_media_tests ~[live]`; test `vt_media_live` runs `vt_media_tests [live]` with LABELS `live`.
  - `VtFFmpeg.cmake` fails configure with a clear message if `VT_FFMPEG_DIR/include/libavcodec/version_major.h` is missing.

- [ ] **Step 1: Write the failing test** (`tests/media/test_ffmpeg.cpp`)

```cpp
TEST_CASE("FFmpeg DLLs match the headers") {
    vt::media::initMedia();
    CHECK(avcodec_version() >> 16 == LIBAVCODEC_VERSION_MAJOR);   // 63
    CHECK(avformat_version() >> 16 == LIBAVFORMAT_VERSION_MAJOR); // 63
    CHECK(avutil_version() >> 16 == LIBAVUTIL_VERSION_MAJOR);     // 61
    CHECK(swscale_version() >> 16 == LIBSWSCALE_VERSION_MAJOR);   // 10
    CHECK(swresample_version() >> 16 == LIBSWRESAMPLE_VERSION_MAJOR); // 7
    for (const char* e : {"ffv1", "prores_ks", "libx264", "aac", "pcm_s16le"})
        CHECK(avcodec_find_encoder_by_name(e) != nullptr);
    CHECK(av_find_input_format("dshow") != nullptr);
    CHECK(av_find_input_format("gdigrab") != nullptr);
}
TEST_CASE("fixtures exist") {
    for (const char* f : {"f30.mkv", "f25_169.mkv", "f60_sar.mkv"})
        CHECK(std::filesystem::file_size(std::filesystem::path(VT_MEDIA_FIXTURES) / f) > 1000);
}
TEST_CASE("averr names an error") { CHECK(vt::media::averr(AVERROR(ENOENT)).find("No such file") != std::string::npos); }
```

- [ ] **Step 2: Run** `cmd /c tools\build.bat`. Expected: configure or compile fails, because `vt_media` does not exist yet.

- [ ] **Step 3: Implement.** `make_fixtures.cmake` runs `ffmpeg.exe -y -v error` through `execute_process` (an argument list, no shell) and skips files that already exist. Commas inside `geq` expressions are avoided, so no filter escaping is needed:
  - `f30.mkv`
    - Video: `-f lavfi -i color=c=black:s=640x480:r=30000/1001:d=5,format=yuv420p,geq=lum=16+N*5-200*floor(N/40):cb=128:cr=128`
    - Audio: `-f lavfi -i sine=frequency=1000:sample_rate=44100:duration=5`
    - Encode: `-c:v ffv1 -c:a pcm_s16le -ac 1`
    - The frame index mod 40 is coded in luma: Y = 16 + 5·(N mod 40).
  - `f25_169.mkv`
    - Video: `color=c=black:s=1280x720:r=25:d=3,format=yuv420p,geq=lum=16+N*5-200*floor(N/40):cb=128:cr=128`
    - Audio: `sine=frequency=440:sample_rate=48000:duration=3`, then `-af aformat=channel_layouts=5.1`
    - Encode: `-c:v ffv1 -c:a pcm_s16le`
  - `f60_sar.mkv`
    - Video: `color=c=black:s=704x480:r=60000/1001:d=2,format=rgb24,geq=r=X-256*floor(X/256):g=Y-256*floor(Y/256):b=64*floor(X/256)+16*floor(Y/256),setsar=10/11`
    - Encode: `-c:v ffv1 -pix_fmt bgr0`, no audio.

- [ ] **Step 4: Run** `cmd /c tools\build.bat`. Expected: `vt_tests`, `vt_render_cli` and `vt_media_tests` pass, and `vt_media_live` is not run.

- [ ] **Step 5: Commit** `build: FFmpeg 9 imported targets, vt_media skeleton, media test fixtures`.

---

### Task 2: Session time and input geometry

**Files:**
- Create: `media/include/vt/media/time.h`, `media/src/time.cpp`, `media/include/vt/media/geometry.h`, `media/src/geometry.cpp`
- Test: `tests/media/test_time_geometry.cpp`

**Interfaces:**
- Produces, `time.h`:
  - `using SessionTime = int64_t;` (1/240000 s)
  - `inline constexpr AVRational kSessionTb{1, 240000};`
  - `constexpr SessionTime tickTime(int64_t n)` = n × 4004
  - `constexpr int64_t samplesBefore(int64_t n)` = n × 4004 / 5
  - `constexpr int samplesInTick(int64_t n)`
  - `SessionTime toSession(int64_t pts, AVRational tb)`, via `av_rescale_q_rnd` with NEAR_INF|PASS_MINMAX
  - `class SessionClock`:
    - `static SessionClock realtime();` (steady_clock, zero at construction)
    - `static SessionClock manual();`
    - `SessionTime now() const;`
    - `void set(SessionTime);` (manual only)
    - Copyable handle; thread-safe `now()`.
- Produces, `geometry.h`:
  - `struct MediaGeometry { vt::Rect inputRect{32, 1, 704, 480}; AVRational pixelAspect{10, 11}; vt::Rect recordRect{24, 1, 720, 480}; };`
  - `vt::Rect fitRect(int srcW, int srcH, AVRational srcSar, const MediaGeometry&)`
    - SAR num or den ≤ 0 counts as 1:1.
    - Display aspect DA = srcW·sar.num / (srcH·sar.den). The box aspect is inputRect.w·pa.num / (inputRect.h·pa.den), which is 4:3.
    - If DA ≥ box: full width, h = round(w·pa.num / (pa.den·DA)).
    - Otherwise: full height, w = round(h·DA·pa.den / pa.num).
    - Centred with floor((box − size)/2) offsets.

- [ ] **Step 1: Write the failing tests**

```cpp
TEST_CASE("tick sample schedule") {
    CHECK(samplesBefore(5) == 4004);
    std::vector<int> n; for (int i = 0; i < 5; ++i) n.push_back(samplesInTick(i));
    CHECK(n == std::vector<int>{800, 801, 801, 801, 801});
    CHECK(samplesBefore(120) == 96096);
    CHECK(samplesBefore(36000) == 28828800);           // 10 minutes, no drift
    CHECK(tickTime(2) == toSession(1, {1001, 30000}));  // 29.97 frame 1 == tick 2
    CHECK(toSession(1, {1, 25}) == 9600);
}
TEST_CASE("manual clock") { auto c = SessionClock::manual(); c.set(4004); CHECK(c.now() == 4004); }
TEST_CASE("fitRect keeps the display aspect") {
    MediaGeometry g;
    CHECK(fitRect(640, 480, {1, 1}, g) == vt::Rect{32, 1, 704, 480});
    CHECK(fitRect(704, 480, {10, 11}, g) == vt::Rect{32, 1, 704, 480});
    CHECK(fitRect(720, 480, {8, 9}, g) == vt::Rect{32, 1, 704, 480});
    CHECK(fitRect(1920, 1080, {1, 1}, g) == vt::Rect{32, 61, 704, 360});
    CHECK(fitRect(1080, 1920, {1, 1}, g) == vt::Rect{235, 1, 297, 480});
    CHECK(fitRect(1920, 1080, {0, 1}, g) == vt::Rect{32, 61, 704, 360});
}
```
(`vt::Rect` needs `operator==`. Add `bool operator==(const Rect&) const = default;` to `engine/include/vt/raster.h`.)

- [ ] **Step 2: Run** `cmd /c tools\build.bat`. Expected: FAIL (undefined).
- [ ] **Step 3: Implement** as specified.
- [ ] **Step 4: Run** `cmd /c tools\build.bat`. Expected: PASS.
- [ ] **Step 5: Commit** `feat(media): session time and input geometry`.

---

### Task 3: Video conversion to the raster

**Files:**
- Create: `media/include/vt/media/video_convert.h`, `media/src/video_convert.cpp`
- Test: `tests/media/test_video_convert.cpp`

**Interfaces:**
- Consumes: `fitRect`, `MediaGeometry`, `vt::Frame`.
- Produces: `class VideoConverter { public: explicit VideoConverter(MediaGeometry = {}); void convert(const AVFrame& src, vt::Frame& out); };`
  - Writes all of `out`: the fitted rect gets the picture, the rest of the raster is opaque black (alpha 255 everywhere).
  - Reuses its `SwsContext` and scratch while the source format, size, SAR and colour stay the same. **No allocation per frame in steady state.**
  - Colour matrix: the frame's `colorspace` if specified, else BT.601 for height ≤ 576 and BT.709 above. Range: the frame's `color_range`, else limited for YUV and full for RGB.
  - Scaling flags: `SWS_BICUBIC | SWS_ACCURATE_RND | SWS_FULL_CHR_H_INT`. When the fitted size equals the source size, only colour is converted (`SWS_POINT`).

- [ ] **Step 1: Write the failing tests.** Build the AVFrames in the test with `av_frame_get_buffer`.

```cpp
TEST_CASE("limited-range BT.601 levels become full-range RGB") {
    // 640x480 yuv420p, Y=235 U=V=128 -> white; Y=16 -> black; Y=81 U=90 V=240 -> red
    CHECK(near(convertSolid(235, 128, 128), {255, 255, 255}, 1));
    CHECK(near(convertSolid(16, 128, 128), {0, 0, 0}, 1));
    CHECK(near(convertSolid(81, 90, 240), {255, 0, 0}, 3));
}
TEST_CASE("704x480 SAR 10:11 RGB is copied pixel for pixel") {
    // bgr0 frame with r = x & 255, g = y & 255, b = (x >> 8) * 64 + (y >> 8) * 16
    // every raster pixel (32 + x, 1 + y) equals the source pixel, alpha 255
}
TEST_CASE("16:9 is letterboxed, portrait pillarboxed, margins black") {
    // 1920x1080 solid white: rows 61..420 inside x 32..735 white, rows 1..60 and 421..480 black,
    // columns 0..31 and 736..767 black, raster rows 0 and 481 black
    // 1080x1920 solid white: columns 235..531 white (rows 1..480), the rest black
}
TEST_CASE("steady state does not reallocate") {
    // convert 10 identical-format frames; out.px.data() unchanged and the converter's
    // context pointer (exposed for tests as contextForTest()) unchanged after the first
}
```

- [ ] **Step 2: Run.** Expected: FAIL.
- [ ] **Step 3: Implement.** Use `sws_scale_frame` into a scratch AVFrame of the fitted size in RGBA, then copy its rows into `out` at the rect, or have `sws_scale` write straight into `out`'s buffer at the rect offset with the raster stride.
- [ ] **Step 4: Run.** Expected: PASS.
- [ ] **Step 5: Commit** `feat(media): convert decoded video into the engine raster`.

---

### Task 4: Audio conversion and the timestamped audio ring

**Files:**
- Create: `media/include/vt/media/audio_convert.h`, `media/src/audio_convert.cpp`
- Test: `tests/media/test_audio.cpp`

**Interfaces:**
- Produces:
  - `class AudioConverter { public: std::span<const float> convert(const AVFrame& src); std::span<const float> convert(const vt::Sound&); };` returns 48 kHz stereo float, interleaved. Mono goes to both channels at unity gain. More than 2 channels are downmixed through swresample's default matrix. Any sample format and rate is accepted. The returned span is valid until the next call.
  - `class AudioRing { public: explicit AudioRing(int64_t capacitySamples = 48000 * 4); void write(int64_t firstSample, std::span<const float> stereo); void read(int64_t firstSample, std::span<float> stereo) const; int64_t endSample() const; };` Positions are absolute 48 kHz session sample indices (`SessionTime / 5`).
    - `read` fills silence for gaps and positions not yet written or already overwritten.
    - A `write` that overlaps earlier data replaces it.
    - Thread-safe for one writer and one reader.

- [ ] **Step 1: Write the failing tests**

```cpp
TEST_CASE("44.1 kHz mono sine becomes 48 kHz stereo at the same pitch") {
    // 1 s of 1 kHz sine at 44100 Hz s16 mono -> 48000 +-32 stereo frames;
    // L == R; zero crossings on L in 1 s: 2000 +-4; peak 0.99..1.01 of the input peak
}
TEST_CASE("5.1 downmix is stereo and not silent") { /* 440 Hz on FL/FR/FC: both outputs RMS > 0.1 */ }
TEST_CASE("8SVX effect sound resamples to its duration") {
    // vt::Sound{rate 8363, 1 ch, 8363 samples} -> 48000 +-32 frames
}
TEST_CASE("ring: gaps are silence, overlaps replace, old data expires") {
    // write [0,800) = 0.5, write [1600,2400) = 0.25; read [0,2400): 0.5, 0, 0.25
    // write [700,900) = -1: read 700..899 == -1
    // capacity 4800: after writing at 10000, read [0,800) is all zero
}
```

- [ ] **Step 2: Run.** Expected: FAIL.
- [ ] **Step 3: Implement.** Use `swr_alloc_set_opts2` (out: `AV_CHANNEL_LAYOUT_STEREO`, `AV_SAMPLE_FMT_FLT`, 48000). Flush the resampler's delay with `swr_convert(..., nullptr, 0)` only for `vt::Sound` (a complete clip).
- [ ] **Step 4: Run.** Expected: PASS.
- [ ] **Step 5: Commit** `feat(media): audio conversion to 48 kHz stereo and a timestamped ring`.

---

### Task 5: File inputs (decode thread, timing, looping, failure)

**Files:**
- Create: `media/include/vt/media/input.h`, `media/src/input.cpp`, `media/src/input_demux.cpp`
- Test: `tests/media/test_input.cpp`

**Interfaces:**
- Consumes: Tasks 2–4.
- Produces (`input.h`):

```cpp
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
};
enum class Wait { No, Block };
class Input {
public:
    Input(const InputConfig&, const MediaGeometry&, SessionClock clock);
    ~Input();                                    // stops and joins the thread
    // The latest picture whose session time <= t, copied into out (skips the copy and returns
    // true if out already holds that picture for this input). False = no picture: caller shows black.
    bool videoAt(SessionTime t, vt::Frame& out, Wait);
    // Samples [first, first + stereo.size()/2) of this input; silence where there are none.
    void audioAt(int64_t firstSample, std::span<float> stereo, Wait);
    InputStatus status() const;
};
```

- **Timing:** when a file opens, its first frame's session time is `clock.now()`, measured at open. Stream pts map through `toSession` plus that offset. Video and audio share the offset, so they stay in sync.
- **Looping:** a looped file restarts with the offset advanced by the file's period, which is the larger end time (`pts + duration`) of its video and audio streams.
- **Look-ahead and blocking:** the decode thread keeps up to 16 converted pictures ahead and 1 s of audio in its `AudioRing`. `Wait::Block` waits until the thread has decoded past `t`, the input ends, or it fails. `Wait::No` returns what is there.
- **Failure:** a file that is missing, unreadable or corrupt gives `State::Error` with `averr` text. It is retried every `retryMs`, so a file that appears later starts playing.
- **Decode errors:** errors mid-file skip the damaged packet and count it.
- **Holding:** `videoAt` keeps returning the last good picture until a newer one is due.

- [ ] **Step 1: Write the failing tests.** Use `SessionClock::manual()` at 0 and `Wait::Block` throughout. `frameIndexOf(frame)` reads raster pixel (384, 241): N mod 40 = round(R·219/1275).

```cpp
TEST_CASE("29.97 file: tick n shows frame n/2") {
    for (int n : {0, 1, 2, 3, 59, 60}) CHECK(frameIndexOf(videoAtTick(in, n)) == (n / 2) % 40);
}
TEST_CASE("25 fps file: tick n shows frame floor(n*25025/60000)") {
    for (int n : {0, 1, 3, 12, 59, 60}) CHECK(frameIndexOf(...) == int64_t(n) * 25025 / 60000 % 40);
    // 0,0,1,5,24,25
}
TEST_CASE("audio follows the file's clock") {
    // f30 audio (1 kHz from t=0): audioAt(samplesBefore(60), 801 frames) non-silent, L == R,
    // zero crossings ~ 2 per ms; the 5.1 file's audio is stereo and non-silent
}
TEST_CASE("looping restarts the picture and the sound") {
    // f30 is 150 frames (5.005 s): tick 300 + 2k shows frame k % 40 again; audio at 5.2 s non-silent
    // loop = false: tick 400 shows frame 149 % 40, audio there is silence, state Ended
}
TEST_CASE("missing, truncated and wrong files fail cleanly") {
    // missing path: videoAt == false, audio silence, state Error, message names the file
    // first 20000 bytes of f30.mkv copied to a temp file: no throw, frames before the cut are
    // shown, then the last frame holds; a text file renamed .mkv: Error
}
TEST_CASE("a missing file that appears is picked up on retry") {
    // retryMs = 100; path missing -> Error; copy f30.mkv there; within 2 s state Running
}
```

- [ ] **Step 2: Run.** Expected: FAIL.
- [ ] **Step 3: Implement.** Open with `avformat_open_input`, take the best video and audio streams, and decode with `avcodec_send_packet`/`receive_frame` and frame threading (`thread_count = 0`). `input_demux.cpp` holds the demux, decode and convert loop shared with Task 9. Interlaced frames get one logged warning per input.
- [ ] **Step 4: Run.** Expected: PASS.
- [ ] **Step 5: Commit** `feat(media): file inputs on their own decode thread with session timing`.

---

### Task 6: Audio mixer (audio follows video)

**Files:**
- Create: `media/include/vt/media/mixer.h`, `media/src/mixer.cpp`
- Test: `tests/media/test_mixer.cpp`

**Interfaces:**
- Consumes: `vt::Switcher` (read-only state), `AudioConverter` for effect sounds, `vt::Sound`.
- Produces:

```cpp
enum class Channel { In1, In2, In3, In4, EffectSound, Master };
struct MixState {                      // what Program shows, from the Switcher after render()
    vt::Source main, preview; int tbar; bool transition, chroma, overlay;
    static MixState of(const vt::Switcher&);
};
struct Meter { float peakDb = -INFINITY, rmsDb = -INFINITY; };   // post-fader, per tick
class AudioMixer {
public:
    void setFaderDb(Channel, float db);   // default 0 dB; <= -96 dB is silence
    void setMute(Channel, bool);
    // Mixes one tick. inputs[i] holds that tick's stereo samples of input i (silence if none);
    // sound != null starts that effect sound at this tick's first sample.
    void process(const MixState&, const std::array<std::span<const float>, 4>& inputs,
                 std::shared_ptr<const vt::Sound> sound, std::span<float> out);
    std::array<Meter, 6> meters() const;  // thread-safe snapshot, indexed by Channel
};
```

**Gain rules** (spec §5.11; my calls marked):
- **Source gains:** take p = tbar/511 when `transition && !chroma && !overlay`.
  - Main = cos(p·π/2), Preview = sin(p·π/2).
  - With no transition, Main = 1 and Preview = 0.
  - *My call:* an engaged ChromaFX shows the recoloured Preview, so the audio is Preview = 1, Main = 0.
  - *My call:* an engaged overlay keeps Main = 1, Preview = 0.
- **Silent sources and shared sources:**
  - BKG, DV1 and DV2 carry no audio.
  - When Main and Preview are the same input, its gain is **1**, not cos + sin.
- **Ramps:** every gain change, a cut included, ramps linearly across the tick's samples. A cut therefore crossfades over one tick (16.7 ms), which is the "short de-clicked switch".
- **Effect sounds:** an effect sound plays from its first sample to its end, even after the effect changes. It is scaled by the EffectSound fader and mute. Overlapping sounds sum.
- **Order:** per-input fader and mute come first, then the source gain, the sum, and the Master fader. There is no limiter; samples are clamped to ±1.

- [ ] **Step 1: Write the failing tests.** Inputs are DC 0.5 on In1 and DC 0.25 on In2; Main = In1 and Preview = In2.

```cpp
TEST_CASE("no transition: Main only")               { /* out == 0.5 after one settling tick */ }
TEST_CASE("mid transition is equal power") {
    // tbar 256: last sample == 0.5*cos(256/511*pi/2) + 0.25*sin(...) +-1e-5
}
TEST_CASE("same source on both buses keeps its level") { /* Main=Preview=In1 at tbar 0,128,256,511: 0.5 */ }
TEST_CASE("BKG and DV sources are silent") { /* Main=Bkg, Preview=DV1, transition at 256: out == 0 */ }
TEST_CASE("a cut ramps over one tick") {
    // tick k: Main=In1; tick k+1: Main=In2 -> first sample ~0.5, last 0.25, monotonic, max step < 0.001
}
TEST_CASE("ChromaFX engaged follows Preview, overlay follows Main") { }
TEST_CASE("effect sound starts at its tick and runs to its end") {
    // Sound 8363 Hz, 4181 samples of 0.5*32767 -> starts at sample 0 of the tick, lasts 24000 +-32
    // output samples, keeps playing across 30 more ticks after a different effect is set
}
TEST_CASE("faders, mute and meters") {
    // In1 fader -6.0206 dB -> 0.25; mute -> 0; full-scale 1 kHz sine: peak 0 +-0.1 dB, rms -3.01 +-0.1
}
```

- [ ] **Step 2: Run.** Expected: FAIL.
- [ ] **Step 3: Implement.** Allocate nothing per tick: preallocate voices and scratch, and cache each effect sound's converted PCM keyed by the `Sound*` it came from.
- [ ] **Step 4: Run.** Expected: PASS.
- [ ] **Step 5: Commit** `feat(media): audio-follows-video mixer with effect sounds and meters`.

---

### Task 7: Program loop (real time and offline)

**Files:**
- Create: `media/include/vt/media/program_loop.h`, `media/src/program_loop.cpp`
- Test: `tests/media/test_program_loop.cpp`

**Interfaces:**
- Consumes: `vt::Switcher`, `Input`, `AudioMixer`, `MixState`, time helpers.
- Produces:

```cpp
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
    void post(std::function<void(vt::Switcher&)>);    // runs at the start of the next tick
    void setInput(int index, const InputConfig&);     // replaces input index (0..3) at the next tick
    AudioMixer& mixer();                              // its setters are thread-safe
    void addSink(ProgramSink*);  void removeSink(ProgramSink*);   // thread-safe
    void start();                // real time on its own thread
    void stop();
    void runOffline(int64_t ticks);  // on the calling thread, inputs read with Wait::Block, delay 0
    InputStatus inputStatus(int index) const;
    LoopStats stats() const;
};
```

**Per tick n**, in this order:
1. Run posted commands.
2. t = tickTime(n) − delay.
3. Call `videoAt(t)` and `audioAt(samplesBefore(n) − delaySamples)` for each input.
4. `sw.render`.
5. `pendingSound()`.
6. `mixer.process(MixState::of(sw), …)`.
7. Call the sinks.
8. `sw.tick()`.

**Pacing and threads:**
- Real time uses absolute deadlines t0 + tickTime(n), waiting on a high-resolution waitable timer (`CreateWaitableTimerExW` with `CREATE_WAITABLE_TIMER_HIGH_RESOLUTION`; fall back to `timeBeginPeriod(1)`).
- A late tick runs at once and counts in `lateTicks`. Ticks are never skipped.
- If the loop falls more than 1 s behind, it re-bases t0 and counts a stall.
- Real-time inputs use `SessionClock::realtime()`, shared by all inputs. Offline inputs use a manual clock held at 0.
- The loop owns four `vt::Frame`s for `SourceFrames`. An input without a picture passes nullptr (black).

- [ ] **Step 1: Write the failing tests** (offline unless stated)

```cpp
TEST_CASE("offline: Program shows In1, then dissolves to In2 over durationTicks") {
    // In1 = f30.mkv, In2 = f25_169.mkv; post: setEffect(makeDissolve()), autoTransition(Fast) at tick 30
    // Recording sink: ticks 0..29 frameIndexOf == (n/2)%40 from In1 (inputRect pixels);
    // after 30 + durationTicks[Fast] ticks the Program equals In2's picture for that tick
}
TEST_CASE("every tick delivers its exact sample count") {
    // runOffline(600): sum of stereo.size()/2 over sinks == samplesBefore(600) == 480480
}
TEST_CASE("audio crossfades with the dissolve") {
    // In1 1 kHz (f30), In2 440 Hz (f25): RMS before ~ In1's, after ~ In2's, mid ~ equal power
}
TEST_CASE("posted commands apply at the next tick, in order") { }
TEST_CASE("an input that fails keeps the Program running") {
    // In1 missing file: 120 ticks delivered, Program black where In1 is Main, inputStatus Error
}
TEST_CASE("effect sound reaches the sinks") {
    // the first manifest row with a non-empty `sound` column (as Plan 1 Ruling 4), via loadEffect:
    // non-silent stereo from the Auto's first tick
}
TEST_CASE("real time keeps 59.94 fps", "[live]") {
    // start(); after 2.002 s ticks == 120 +-3; lateTicks small; no stalls
}
```

- [ ] **Step 2: Run.** Expected: FAIL.
- [ ] **Step 3: Implement** as specified.
- [ ] **Step 4: Run.** Expected: PASS. The `[live]` case passes under `tools\live.bat`.
- [ ] **Step 5: Commit** `feat(media): program loop driving the Switcher, inputs, mixer and sinks`.

---

### Task 8: Recorder

**Files:**
- Create: `media/include/vt/media/recorder.h`, `media/src/recorder.cpp`
- Test: `tests/media/test_recorder.cpp`

**Interfaces:**
- Consumes: `ProgramSink`, `MediaGeometry::recordRect`.
- Produces:

```cpp
enum class RecordFormat { H264Mkv, H264Mp4, Ffv1Mkv, ProResMov };
struct RecorderSettings {
    std::filesystem::path path;
    RecordFormat format = RecordFormat::H264Mkv;
    int crf = 16;                       // H.264 (libx264, preset veryfast, yuv420p, High)
    int audioKbps = 192;                // AAC for H.264 formats; FFV1/ProRes use pcm_s16le
    MediaGeometry geometry;
};
struct RecorderStatus { bool recording = false; std::string error; int64_t framesWritten = 0, framesDropped = 0; };
class Recorder : public ProgramSink {
public:
    bool start(const RecorderSettings&, std::string* error);   // opens the file and encoders
    void stop();                                               // drains, flushes, writes trailer
    RecorderStatus status() const;
    void onProgram(int64_t tick, const vt::Frame&, std::span<const float>) override;
};
```

**Formats:**
- The file is `recordRect` cropped from the raster: 720×480, SAR 10:11, time base 1001/60000, constant frame rate.
- Video pts = ticks since the first recorded tick. Audio pts = samples since then.
- H.264 is yuv420p and ProRes 422 HQ (`prores_ks` profile 3) is yuv422p10le, both converted with swscale at the same size, tagged SMPTE 170M (primaries, trc, matrix), limited range. FFV1 is bgr0, an exact copy of the pixels.

**Threads and failures:**
- Encoding runs on the recorder's own thread from a queue of copied frames and audio. The queue is capped at 120 entries; beyond that new frames are dropped and counted. Audio is never dropped; a dropped frame's audio still goes in.
- Errors (open, write, disk full) set `status().error`, stop accepting frames and never throw to the loop.
- `stop()` after an error still writes the trailer when possible, so MKV stays playable.

- [ ] **Step 1: Write the failing tests.** Drive the recorder by calling `onProgram` directly with synthetic frames, and check results by reading the file back with libavformat in the test.

```cpp
TEST_CASE("FFV1 recording is pixel exact and well formed") {
    // 120 ticks of a coordinate-coded raster + 1 kHz tone -> probe: 1 video stream 720x480,
    // SAR 10/11, avg_frame_rate 60000/1001, 120 frames; 1 audio stream 48000 Hz stereo with
    // 96096 samples total; decoded frame 10 == raster crop at (24,1) of tick 10 exactly
}
TEST_CASE("H.264 MKV, H.264 MP4 and ProRes MOV open and play back") {
    // 60 ticks each: probe codec names h264/h264/prores, 60 frames, colour tags smpte170m,
    // audio aac/aac/pcm_s16le, duration 1.001 s +-1 frame
}
TEST_CASE("an unwritable path fails at start without throwing") {
    // path in a directory that does not exist -> start false, error non-empty
}
TEST_CASE("a write failure mid-recording is reported and stop leaves a playable file") {
    // test seam: RecorderSettings::failAfterBytesForTest (only in test builds) -> status().error set,
    // later onProgram calls are ignored, the file probes with framesWritten frames
}
TEST_CASE("a full queue drops frames, never audio") {
    // test seam pauses the encoder thread; 200 onProgram calls -> framesDropped == 80,
    // audio sample total still samplesBefore(200)
}
```

- [ ] **Step 2: Run.** Expected: FAIL.
- [ ] **Step 3: Implement.** Test seams exist only under `#ifdef VT_MEDIA_TEST_SEAMS`. `media/CMakeLists.txt` builds the same sources twice: `vt_media` without the define, and `vt_media_testable` with it for `vt_media_tests`. Release code has no seam branches.
- [ ] **Step 4: Run.** Expected: PASS.
- [ ] **Step 5: Commit** `feat(media): Program recorder (H.264, FFV1, ProRes) with mixed audio`.

---

### Task 9: Devices, screen capture, reconnecting

**Files:**
- Create: `media/include/vt/media/devices.h`, `media/src/devices.cpp`
- Modify: `media/src/input.cpp`, `media/src/input_demux.cpp`
- Test: `tests/media/test_devices.cpp`

**Interfaces:**
- Produces:
  - `struct DeviceInfo { std::string name, id; };` (id is dshow's alternative name, which is unique)
  - `enum class DeviceKind { Video, Audio };`
  - `std::vector<DeviceInfo> listDevices(DeviceKind, std::string* error = nullptr);` uses `avdevice_list_input_sources(av_find_input_format("dshow"), …)`, filtered by media type. It returns empty on error, without throwing.
- Input kinds:
  - **Device:** opens dshow URL `video=<videoDevice>[:audio=<audioDevice>]`, or `audio=…` alone, with `options` (e.g. `video_size`, `framerate`, `rtbufsize` default `"100M"`).
  - **Screen:** opens gdigrab `desktop` or `title=<window>` (`framerate` default `"60000/1001"`), plus a separate dshow audio context when `audioDevice` is set.
  - **Clock:** live streams map pts → session time with an offset taken at the first frame's arrival (`clock.now()`). They re-anchor when |mapped − arrival| > 48000 units (200 ms) and log it. A paired audio device uses its own mapper on the same clock.
- **Reconnect:** a live source that errors or stops delivering for 2 s goes to `State::Error` and reopens every `retryMs`.

- [ ] **Step 1: Write the failing tests**

```cpp
TEST_CASE("device lists never throw") { std::string e; auto v = listDevices(DeviceKind::Video, &e); SUCCEED(); }
TEST_CASE("a device that does not exist fails cleanly") {
    // Device "video=vtoast-no-such-camera": Error within 3 s, videoAt false, audio silence
}
TEST_CASE("desktop capture delivers frames", "[live]") {
    // Screen, window "": Running within 3 s; a picture fitted inside inputRect; framesDecoded grows
}
TEST_CASE("first real camera opens", "[live]") {
    // if listDevices(Video) is empty: SKIP("no camera"); else Running within 5 s
}
```

- [ ] **Step 2: Run.** Expected: FAIL.
- [ ] **Step 3: Implement.** Reuse Task 5's demux/decode/convert loop. A dshow device that offers only compressed formats (MJPEG) is decoded like a file stream.
- [ ] **Step 4: Run** `cmd /c tools\build.bat`, then `cmd /c tools\live.bat`. Expected: PASS. The camera case may SKIP.
- [ ] **Step 5: Commit** `feat(media): DirectShow devices, screen capture and reconnecting live inputs`.

---

### Task 10: `vt_show` CLI and the end-to-end test

**Files:**
- Create: `cli/vt_show.cpp`, `tests/media/test_vt_show.cmake`
- Modify: `cli/CMakeLists.txt`, `tests/media/CMakeLists.txt`

**Interfaces:**
- Consumes: everything above.
- Produces: `vt_show`

```
vt_show --in1 <spec> [--in2 ..] [--in3 ..] [--in4 ..] --out <file> [--format h264|mp4|ffv1|prores]
        [--effect <crouton>|dissolve|cut] [--speed slow|medium|fast|variable]
        [--auto-at <s>]... [--take-at <s>]... [--main <src>] [--preview <src>]
        [--seconds <s>] [--realtime] [--list-devices]
  <spec>: file:<path> | dshow:<video name>[|<audio name>] | screen[:<window title>]
  <src>:  in1..in4 | dv1 | dv2 | bkg
```

- **Defaults:** Main = in1, Preview = in2, effect = dissolve, speed = the effect's default.
- **Offline (default):** runs `runOffline(round(seconds × 60000/1001))` ticks. Events convert from seconds to ticks with `round(s × 60000 / 1001)`.
- **Real time (`--realtime`):** runs against the clock until `--seconds` or Ctrl+C, printing input status and `LoopStats` once a second. This is the manual test bench for live devices until Plan 3.
- **Exit codes:** non-zero with a message for a bad spec, an unloadable effect or a recorder error.

- [ ] **Step 1: Write the failing CTest script** `test_vt_show.cmake`, which needs `media_fixtures`. Run:

```
vt_show --in1 file:f30.mkv --in2 file:f25_169.mkv --effect "<VT_DATA_ROOT>/4.2/Effects/.../Page Peel !"
        --speed fast --auto-at 1.0 --seconds 3 --format ffv1 --out show.mkv
```

Then check with `ffprobe.exe -v error -count_frames -show_streams -of json show.mkv`:
- video 720×480, `nb_read_frames` = round(3 × 60000/1001) = 180, `avg_frame_rate` 60000/1001, `sample_aspect_ratio` 10:11;
- audio 48000 Hz, 2 channels.

Also run `vt_show --list-devices`: exit 0. Find the Page Peel ! path with the same lookup `test_cli.cmake` uses.

- [ ] **Step 2: Run** `cmd /c tools\build.bat`. Expected: FAIL (no `vt_show`).
- [ ] **Step 3: Implement** `vt_show`.
- [ ] **Step 4: Run** `cmd /c tools\build.bat`. Expected: all tests PASS.
- [ ] **Step 5: Commit** `feat(cli): vt_show — scripted or live shows recorded to a file`.

---

## Notes for Plan 3

The Qt app will:
- run `ProgramLoop::start()`;
- drive the Switcher only through `post()`;
- add a monitor `ProgramSink` that copies the frame for display, plus an audio output sink (Qt `QAudioSink`/WASAPI), because FFmpeg has no audio output device;
- read `mixer().meters()` and `inputStatus()` for the mixer dock.

Plan 3 must also decide how to display 10:11 pixels at whole-number zoom, since square-pixel display makes 4:3 look 10% too wide. That is an owner question for Plan 3.

Interfaces and constraints Plan 3 inherits from Plan 2 (final review):
- **Preview monitor.** `ProgramSink` carries only the Program picture. The Preview monitor needs the Preview source for each tick, so Plan 3 must add an interface for it, for example a sink variant that also receives the tick's `SourceFrames`, or a Preview render on the loop thread.
- **Audio output drift.** The loop paces ticks on the steady clock; the sound card runs on its own clock. The audio output sink must absorb the drift between them (an elastic buffer that drops or repeats a few samples, or resamples slightly). Otherwise it will underrun or grow without bound over a long show.
- **FFmpeg DLLs.** The Qt executable links `vt_media`, so it must call `vt_copy_ffmpeg_dlls(<target>)` like every other executable that does.
- **Log sink.** `vt::setLogSink`'s callback runs under a global lock, from decoder, capture and recorder threads as well as the loop thread. The UI's log sink must not block on the UI thread (no blocking invoke); it should queue the line and return.
- **Input status.** `inputStatus()` reads Idle until the first run opens the inputs (they open on the first tick after `start()`); poll it after starting. Every `start()` reopens the inputs on a fresh clock.
- **Meters.** `mixer().meters()` returns the peak and the RMS since the previous call, so exactly one reader (the mixer dock's timer) should call it.
- **Stopping a recording.** `Recorder::stop()` blocks while the queue drains (up to 120 frames), so call it off the UI thread or accept a short wait.
