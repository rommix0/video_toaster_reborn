#include "catch_amalgamated.hpp"
#include "vt/media/ffmpeg.h"
#include "vt/media/recorder.h"
#include "vt/media/time.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <future>
#include <numbers>
#include <string>
#include <thread>
#include <vector>

using namespace vt::media;
namespace fs = std::filesystem;
using namespace std::chrono_literals;

namespace {

fs::path tempFile(const char* name) {
    const fs::path dir = fs::temp_directory_path() / "vt_recorder_tests";
    fs::create_directories(dir);
    const fs::path p = dir / name;
    fs::remove(p);
    return p;
}

// A raster that codes its own coordinates and the tick, so any shift or mix-up shows.
vt::Frame makeFrame(int64_t tick) {
    vt::Frame f;
    for (int y = 0; y < vt::kRasterH; ++y)
        for (int x = 0; x < vt::kRasterW; ++x)
            f.at(x, y) = {uint8_t(x * 3 + tick), uint8_t(y * 5 + tick * 7), uint8_t((x ^ y) + tick * 3), 255};
    return f;
}

// The 1 kHz tone, continuous across ticks.
std::vector<float> makeTone(int64_t tick) {
    const int n = samplesInTick(tick);
    std::vector<float> a(size_t(n) * 2);
    for (int i = 0; i < n; ++i) {
        const double s = double(samplesBefore(tick) + i);
        a[size_t(i) * 2] = a[size_t(i) * 2 + 1] = float(0.5 * std::sin(2 * std::numbers::pi * 1000.0 * s / 48000.0));
    }
    return a;
}

void feed(Recorder& r, int64_t first, int64_t count) {
    for (int64_t t = first; t < first + count; ++t) {
        const vt::Frame f = makeFrame(t);
        const std::vector<float> a = makeTone(t);
        r.onProgram(t, f, a);
    }
}

struct Probe {
    int videoStreams = 0, audioStreams = 0;
    int w = 0, h = 0, audioRate = 0, audioChannels = 0;
    AVRational sar{}, avgFps{}, rFps{};
    std::string vcodec, acodec;
    AVColorPrimaries primaries = AVCOL_PRI_UNSPECIFIED;
    AVColorTransferCharacteristic trc = AVCOL_TRC_UNSPECIFIED;
    AVColorSpace space = AVCOL_SPC_UNSPECIFIED;
    AVColorRange range = AVCOL_RANGE_UNSPECIFIED;
    int64_t videoFrames = 0, audioSamples = 0;
    double duration = 0;
    double audioPeak = -1;                 // of pcm_s16le audio, in full scale units
    std::vector<uint8_t> frameRgba;        // the decoded video frame number `wantFrame`, as RGBA
    int centreY = -1, centreU = -1, centreV = -1;   // of the first frame when it is yuv420p
};

Probe probe(const fs::path& path, int64_t wantFrame = -1) {
    Probe p;
    AVFormatContext* raw = nullptr;
    const std::string s = path.string();
    REQUIRE(avformat_open_input(&raw, s.c_str(), nullptr, nullptr) == 0);
    FormatCtxPtr fmt(raw);
    REQUIRE(avformat_find_stream_info(fmt.get(), nullptr) >= 0);
    p.duration = double(fmt->duration) / AV_TIME_BASE;

    CodecCtxPtr vdec, adec;
    int vi = -1, ai = -1;
    for (unsigned i = 0; i < fmt->nb_streams; ++i) {
        AVStream* st = fmt->streams[i];
        const AVCodecParameters* cp = st->codecpar;
        if (cp->codec_type == AVMEDIA_TYPE_VIDEO) {
            ++p.videoStreams;
            vi = int(i);
            p.w = cp->width;
            p.h = cp->height;
            p.sar = st->sample_aspect_ratio.num ? st->sample_aspect_ratio : cp->sample_aspect_ratio;
            p.avgFps = st->avg_frame_rate;
            p.rFps = st->r_frame_rate;
            p.vcodec = avcodec_get_name(cp->codec_id);
            p.primaries = cp->color_primaries;
            p.trc = cp->color_trc;
            p.space = cp->color_space;
            p.range = cp->color_range;
            vdec.reset(avcodec_alloc_context3(avcodec_find_decoder(cp->codec_id)));
            avcodec_parameters_to_context(vdec.get(), cp);
            vdec->pkt_timebase = st->time_base;
            REQUIRE(avcodec_open2(vdec.get(), nullptr, nullptr) == 0);
        } else if (cp->codec_type == AVMEDIA_TYPE_AUDIO) {
            ++p.audioStreams;
            ai = int(i);
            p.audioRate = cp->sample_rate;
            p.audioChannels = cp->ch_layout.nb_channels;
            p.acodec = avcodec_get_name(cp->codec_id);
            adec.reset(avcodec_alloc_context3(avcodec_find_decoder(cp->codec_id)));
            avcodec_parameters_to_context(adec.get(), cp);
            adec->pkt_timebase = st->time_base;
            REQUIRE(avcodec_open2(adec.get(), nullptr, nullptr) == 0);
        }
    }

    PacketPtr pkt(av_packet_alloc());
    FramePtr frame(av_frame_alloc());
    SwsPtr sws;
    FramePtr rgba;
    auto drain = [&](AVCodecContext* c, int stream) {
        while (avcodec_receive_frame(c, frame.get()) == 0) {
            if (stream == vi) {
                if (p.videoFrames == 0 && frame->format == AV_PIX_FMT_YUV420P) {
                    p.centreY = frame->data[0][ptrdiff_t(frame->height / 2) * frame->linesize[0] + frame->width / 2];
                    p.centreU = frame->data[1][ptrdiff_t(frame->height / 4) * frame->linesize[1] + frame->width / 4];
                    p.centreV = frame->data[2][ptrdiff_t(frame->height / 4) * frame->linesize[2] + frame->width / 4];
                }
                if (p.videoFrames == wantFrame) {
                    rgba.reset(av_frame_alloc());
                    rgba->format = AV_PIX_FMT_RGBA;
                    rgba->width = frame->width;
                    rgba->height = frame->height;
                    REQUIRE(av_frame_get_buffer(rgba.get(), 0) == 0);
                    sws.reset(sws_alloc_context());
                    sws->flags = SWS_POINT;
                    REQUIRE(sws_scale_frame(sws.get(), rgba.get(), frame.get()) >= 0);
                    p.frameRgba.resize(size_t(p.w) * size_t(p.h) * 4);
                    for (int y = 0; y < p.h; ++y)
                        std::memcpy(&p.frameRgba[size_t(y) * size_t(p.w) * 4], rgba->data[0] + ptrdiff_t(y) * rgba->linesize[0],
                                    size_t(p.w) * 4);
                }
                ++p.videoFrames;
            } else {
                p.audioSamples += frame->nb_samples;
                if (frame->format == AV_SAMPLE_FMT_S16) {
                    const auto* d = reinterpret_cast<const int16_t*>(frame->data[0]);
                    for (int i = 0; i < frame->nb_samples * frame->ch_layout.nb_channels; ++i)
                        p.audioPeak = std::max(p.audioPeak, std::abs(double(d[i])) / 32768.0);
                }
            }
            av_frame_unref(frame.get());
        }
    };
    while (av_read_frame(fmt.get(), pkt.get()) >= 0) {
        AVCodecContext* c = pkt->stream_index == vi ? vdec.get() : pkt->stream_index == ai ? adec.get() : nullptr;
        if (c) {
            REQUIRE(avcodec_send_packet(c, pkt.get()) == 0);
            drain(c, pkt->stream_index);
        }
        av_packet_unref(pkt.get());
    }
    if (vdec) { avcodec_send_packet(vdec.get(), nullptr); drain(vdec.get(), vi); }
    if (adec) { avcodec_send_packet(adec.get(), nullptr); drain(adec.get(), ai); }
    return p;
}

RecorderSettings settingsFor(const fs::path& path, RecordFormat f) {
    RecorderSettings s;
    s.path = path;
    s.format = f;
    return s;
}

bool eventually(auto&& cond, std::chrono::milliseconds limit = 5000ms) {
    const auto end = std::chrono::steady_clock::now() + limit;
    while (!cond()) {
        if (std::chrono::steady_clock::now() > end) return false;
        std::this_thread::sleep_for(5ms);
    }
    return true;
}

void checkFrameRate(AVRational fps) {
    // Matroska keeps 1 ms timestamps and reads a rounded rate back (e.g. 19001/317).
    REQUIRE(fps.den > 0);
    REQUIRE(std::abs(av_q2d(fps) - 60000.0 / 1001.0) < 0.01);
}

} // namespace

TEST_CASE("FFV1 recording is pixel exact and well formed", "[recorder]") {
    initMedia();
    const fs::path path = tempFile("ffv1.mkv");
    Recorder rec;
    std::string err;
    REQUIRE(rec.start(settingsFor(path, RecordFormat::Ffv1Mkv), &err));
    REQUIRE(err.empty());
    REQUIRE(rec.status().recording);
    feed(rec, 0, 120);
    rec.stop();

    const RecorderStatus st = rec.status();
    CHECK_FALSE(st.recording);
    CHECK(st.error.empty());
    CHECK(st.framesWritten == 120);
    CHECK(st.framesDropped == 0);

    const Probe p = probe(path, 10);
    CHECK(p.videoStreams == 1);
    CHECK(p.w == 720);
    CHECK(p.h == 480);
    CHECK(p.sar.num == 10);
    CHECK(p.sar.den == 11);
    checkFrameRate(p.avgFps);
    CHECK(p.vcodec == "ffv1");
    CHECK(p.videoFrames == 120);
    CHECK(p.audioStreams == 1);
    CHECK(p.audioRate == 48000);
    CHECK(p.audioChannels == 2);
    CHECK(p.acodec == "pcm_s16le");
    CHECK(p.audioSamples == 96096);
    CHECK(p.audioPeak == Catch::Approx(0.5).margin(0.01));

    // Frame 10 is the raster crop at (24,1) of tick 10, exactly.
    REQUIRE(p.frameRgba.size() == size_t(720) * 480 * 4);
    const vt::Frame expect = makeFrame(10);
    size_t wrong = 0;
    for (int y = 0; y < 480; ++y)
        for (int x = 0; x < 720; ++x) {
            const vt::Rgba8 e = expect.at(24 + x, 1 + y);
            const uint8_t* g = &p.frameRgba[(size_t(y) * 720 + size_t(x)) * 4];
            if (g[0] != e.r || g[1] != e.g || g[2] != e.b) ++wrong;
        }
    CHECK(wrong == 0);
    fs::remove(path);
}

TEST_CASE("H.264 MKV, H.264 MP4 and ProRes MOV open and play back", "[recorder]") {
    initMedia();
    struct Case { const char* file; RecordFormat format; const char* vcodec; const char* acodec; };
    const Case cases[] = {
        {"h264.mkv", RecordFormat::H264Mkv, "h264", "aac"},
        {"h264.mp4", RecordFormat::H264Mp4, "h264", "aac"},
        {"prores.mov", RecordFormat::ProResMov, "prores", "pcm_s16le"},
    };
    for (const Case& c : cases) {
        INFO(c.file);
        const fs::path path = tempFile(c.file);
        Recorder rec;
        std::string err;
        REQUIRE(rec.start(settingsFor(path, c.format), &err));
        feed(rec, 0, 60);
        rec.stop();
        CHECK(rec.status().error.empty());
        CHECK(rec.status().framesWritten == 60);

        const Probe p = probe(path);
        CHECK(p.videoStreams == 1);
        CHECK(p.w == 720);
        CHECK(p.h == 480);
        CHECK(p.sar.num == 10);
        CHECK(p.sar.den == 11);
        checkFrameRate(p.avgFps);
        CHECK(p.vcodec == c.vcodec);
        CHECK(p.videoFrames == 60);
        CHECK(p.primaries == AVCOL_PRI_SMPTE170M);
        CHECK(p.trc == AVCOL_TRC_SMPTE170M);
        CHECK(p.space == AVCOL_SPC_SMPTE170M);
        CHECK(p.range == AVCOL_RANGE_MPEG);
        CHECK(p.audioStreams == 1);
        CHECK(p.acodec == c.acodec);
        CHECK(p.audioRate == 48000);
        CHECK(p.audioChannels == 2);
        // 1.001 s +-1 frame. The Matroska muxer also adds AAC's codec delay (1024 samples) to the
        // segment duration it writes, so an AAC MKV may read that much longer.
        const double margin = 1001.0 / 60000.0 + (c.format == RecordFormat::H264Mkv ? 1024.0 / 48000.0 : 0.0);
        CHECK(p.duration == Catch::Approx(1.001).margin(margin));
        fs::remove(path);
    }
}

TEST_CASE("H.264 colour is BT.601 limited range", "[recorder]") {
    initMedia();
    const fs::path path = tempFile("colour.mkv");
    RecorderSettings s = settingsFor(path, RecordFormat::H264Mkv);
    Recorder rec;
    std::string err;
    REQUIRE(rec.start(s, &err));
    vt::Frame red;
    std::fill(red.px.begin(), red.px.end(), vt::Rgba8{255, 0, 0, 255});
    for (int64_t t = 0; t < 4; ++t) rec.onProgram(t, red, makeTone(t));
    rec.stop();
    REQUIRE(rec.status().error.empty());

    const Probe p = probe(path);
    // Full-range (255,0,0) in BT.601 limited range: Y 81.5, Cb 90.3, Cr 240.
    CHECK(std::abs(p.centreY - 82) <= 2);
    CHECK(std::abs(p.centreU - 90) <= 2);
    CHECK(std::abs(p.centreV - 240) <= 2);
    fs::remove(path);
}

TEST_CASE("an unwritable path fails at start without throwing", "[recorder]") {
    initMedia();
    Recorder rec;
    std::string err;
    const fs::path path = fs::temp_directory_path() / "vt_recorder_tests" / "no_such_dir" / "deeper" / "out.mkv";
    bool started = true;
    REQUIRE_NOTHROW(started = rec.start(settingsFor(path, RecordFormat::H264Mkv), &err));
    CHECK_FALSE(started);
    CHECK_FALSE(err.empty());
    CHECK_FALSE(rec.status().recording);
    CHECK_FALSE(rec.status().error.empty());
    // A stopped or never started recorder takes frames and stop() calmly.
    feed(rec, 0, 2);
    REQUIRE_NOTHROW(rec.stop());
    CHECK(rec.status().framesWritten == 0);
}

TEST_CASE("a write failure mid-recording is reported and stop leaves a playable file", "[recorder]") {
    initMedia();
    const fs::path path = tempFile("failing.mkv");
    RecorderSettings s = settingsFor(path, RecordFormat::Ffv1Mkv);
    s.blockWhenFull = true;
    s.failAfterBytesForTest = 400000;
    Recorder rec;
    std::string err;
    REQUIRE(rec.start(s, &err));
    int64_t fed = 0;
    while (fed < 400 && rec.status().error.empty()) {
        feed(rec, fed, 1);
        ++fed;
        std::this_thread::sleep_for(2ms);
    }
    REQUIRE(eventually([&] { return !rec.status().error.empty(); }));
    CHECK_FALSE(rec.status().recording);
    const int64_t written = rec.status().framesWritten;
    REQUIRE(written > 0);
    REQUIRE(written < fed);

    feed(rec, fed, 5);   // ignored: nothing queued, nothing counted
    rec.stop();
    CHECK(rec.status().framesWritten == written);
    CHECK(rec.status().framesDropped == 0);
    CHECK_FALSE(rec.status().error.empty());

    const Probe p = probe(path);
    CHECK(p.videoFrames == written);
    fs::remove(path);
}

TEST_CASE("a full queue drops frames, never audio", "[recorder]") {
    initMedia();
    const fs::path path = tempFile("full.mkv");
    Recorder rec;
    std::string err;
    REQUIRE(rec.start(settingsFor(path, RecordFormat::Ffv1Mkv), &err));
    rec.setEncoderPausedForTest(true);
    feed(rec, 0, 200);
    CHECK(rec.status().framesDropped == 80);
    rec.setEncoderPausedForTest(false);
    rec.stop();
    CHECK(rec.status().error.empty());
    CHECK(rec.status().framesWritten == 120);
    CHECK(rec.status().framesDropped == 80);

    const Probe p = probe(path);
    CHECK(p.videoFrames == 120);
    CHECK(p.audioSamples == samplesBefore(200));
    fs::remove(path);
}

TEST_CASE("blockWhenFull makes the producer wait instead of dropping", "[recorder]") {
    initMedia();
    const fs::path path = tempFile("blocking.mkv");
    RecorderSettings s = settingsFor(path, RecordFormat::Ffv1Mkv);
    s.blockWhenFull = true;
    Recorder rec;
    std::string err;
    REQUIRE(rec.start(s, &err));
    rec.setEncoderPausedForTest(true);

    std::atomic<int> calls{0};
    std::thread producer([&] {
        for (int64_t t = 0; t < 130; ++t) {
            feed(rec, t, 1);
            ++calls;
        }
    });
    REQUIRE(eventually([&] { return calls.load() >= 120; }));
    std::this_thread::sleep_for(200ms);
    CHECK(calls.load() == 120);          // the 121st call is waiting for room
    rec.setEncoderPausedForTest(false);
    producer.join();
    CHECK(calls.load() == 130);
    rec.stop();

    CHECK(rec.status().error.empty());
    CHECK(rec.status().framesDropped == 0);
    CHECK(rec.status().framesWritten == 130);
    const Probe p = probe(path);
    CHECK(p.videoFrames == 130);
    CHECK(p.audioSamples == samplesBefore(130));
    fs::remove(path);
}

TEST_CASE("stop returns a blocked producer promptly", "[recorder]") {
    initMedia();
    const fs::path path = tempFile("stopblocked.mkv");
    RecorderSettings s = settingsFor(path, RecordFormat::Ffv1Mkv);
    s.blockWhenFull = true;
    Recorder rec;
    std::string err;
    REQUIRE(rec.start(s, &err));
    rec.setEncoderPausedForTest(true);

    std::atomic<int> calls{0};
    std::atomic<bool> done{false};
    std::thread producer([&] {
        for (int64_t t = 0; t < 130; ++t) {
            feed(rec, t, 1);
            ++calls;
        }
        done = true;
    });
    REQUIRE(eventually([&] { return calls.load() >= 120; }));
    std::this_thread::sleep_for(100ms);
    REQUIRE_FALSE(done.load());

    auto stopper = std::async(std::launch::async, [&] { rec.stop(); });
    CHECK(eventually([&] { return done.load(); }, 1000ms));
    stopper.get();
    producer.join();
    CHECK(rec.status().framesDropped == 0);
    fs::remove(path);
}

TEST_CASE("every format encodes well ahead of real time", "[recorder]") {
    // 59.94 fps is real time; the encoders must keep ahead with room to spare. They are
    // threaded: on one thread ProRes made ~48 fps and FFV1 ~99 fps at 720x480 (Ryzen 5600X),
    // threaded both make several hundred. The bar is 2x real time; a busy machine can miss it.
    initMedia();
    constexpr int kFrames = 240;
    std::vector<vt::Frame> frames;
    std::vector<std::vector<float>> tones;
    for (int64_t t = 0; t < 8; ++t) {
        frames.push_back(makeFrame(t));
        tones.push_back(makeTone(t));
    }
    struct Case { const char* file; RecordFormat format; };
    for (const Case& c : {Case{"speed_h264.mkv", RecordFormat::H264Mkv}, Case{"speed_ffv1.mkv", RecordFormat::Ffv1Mkv},
                          Case{"speed_prores.mov", RecordFormat::ProResMov}}) {
        const fs::path path = tempFile(c.file);
        RecorderSettings s = settingsFor(path, c.format);
        s.blockWhenFull = true;
        Recorder rec;
        std::string err;
        REQUIRE(rec.start(s, &err));
        const auto t0 = std::chrono::steady_clock::now();
        for (int64_t t = 0; t < kFrames; ++t) rec.onProgram(t, frames[size_t(t % 8)], tones[size_t(t % 2)]);
        rec.stop();
        const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        const double fps = kFrames / secs;
        INFO(c.file << ": " << kFrames << " frames in " << secs << " s = " << fps << " fps (bar: 2x real time = 120 fps; a loaded machine can miss it)");
        CHECK(rec.status().error.empty());
        CHECK(rec.status().framesWritten == kFrames);
        CHECK(fps >= 2 * 60000.0 / 1001.0);
        fs::remove(path);
    }
}
