#include "catch_amalgamated.hpp"
#include "vt/log.h"
#include "vt/media/input.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

using namespace vt::media;
namespace fs = std::filesystem;
using State = InputStatus::State;

namespace {

std::string fixture(const char* name) { return (fs::path(VT_MEDIA_FIXTURES) / name).string(); }

InputConfig fileConfig(const std::string& path, bool loop = true) {
    InputConfig c;
    c.kind = InputKind::File;
    c.path = path;
    c.loop = loop;
    return c;
}

// The fixtures' luma code: Y = 16 + 5 * (N mod 40), read back from the RGB raster centre.
int frameIndexOf(const vt::Frame& f) { return int(std::lround(f.at(384, 241).r * 219.0 / 1275.0)); }

vt::Frame videoAtTick(Input& in, int64_t n) {
    vt::Frame f;
    REQUIRE(in.videoAt(tickTime(n), f, Wait::Block));
    return f;
}

std::vector<float> audioAt(Input& in, int64_t first, int frames) {
    std::vector<float> v(size_t(frames) * 2, 1.0f); // dirty, so silence must be written
    in.audioAt(first, v, Wait::Block);
    return v;
}

float peak(const std::vector<float>& v) {
    float p = 0;
    for (float x : v) p = std::max(p, std::fabs(x));
    return p;
}

int crossings(const std::vector<float>& v) {
    int n = 0;
    for (size_t i = 1; i < v.size() / 2; ++i) n += (v[(i - 1) * 2] < 0) != (v[i * 2] < 0);
    return n;
}

bool leftEqualsRight(const std::vector<float>& v) {
    for (size_t i = 0; i < v.size(); i += 2)
        if (v[i] != v[i + 1]) return false;
    return true;
}

bool waitForState(const Input& in, State want, std::chrono::milliseconds limit) {
    const auto until = std::chrono::steady_clock::now() + limit;
    while (in.status().state != want) {
        if (std::chrono::steady_clock::now() > until) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return true;
}

// A scratch file path, unique to this run (test processes may run side by side), removed when
// the test ends.
struct TempPath {
    fs::path path;
    explicit TempPath(const char* name)
        : path(fs::temp_directory_path() /
               (std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "_" + name)) {}
    ~TempPath() { fs::remove(path); }
    std::string str() const { return path.string(); }
};

// Captures warnings (FFmpeg's complaints about damaged files) so test output stays clean.
struct QuietLog {
    std::vector<std::string> lines;
    QuietLog() { vt::setLogSink([this](std::string_view m) { lines.emplace_back(m); }); }
    ~QuietLog() { vt::setLogSink({}); }
};

} // namespace

TEST_CASE("29.97 file: tick n shows frame n/2") {
    Input in(fileConfig(fixture("f30.mkv")), {}, SessionClock::manual());
    for (int n : {0, 1, 2, 3, 59, 60}) {
        INFO("tick " << n);
        CHECK(frameIndexOf(videoAtTick(in, n)) == (n / 2) % 40);
    }
    const InputStatus st = in.status();
    CHECK(st.state == State::Running);
    CHECK(st.width == 640);
    CHECK(st.height == 480);
    CHECK(st.hasAudio);
    CHECK(st.framesDecoded >= 31);
    CHECK(st.framesLate == 0);
}

TEST_CASE("25 fps file: tick n shows frame floor(n*25025/60000)") {
    Input in(fileConfig(fixture("f25_169.mkv")), {}, SessionClock::manual());
    for (int n : {0, 1, 3, 12, 59, 60}) { // 0, 0, 1, 5, 24, 25
        INFO("tick " << n);
        CHECK(frameIndexOf(videoAtTick(in, n)) == int64_t(n) * 25025 / 60000 % 40);
    }
}

TEST_CASE("audio follows the file's clock") {
    {
        Input in(fileConfig(fixture("f30.mkv")), {}, SessionClock::manual());
        const auto a = audioAt(in, samplesBefore(60), 801); // 1.001 s .. 1.018 s of a 1 kHz tone
        CHECK(peak(a) > 0.1f); // lavfi sine: amplitude 1/8
        CHECK(leftEqualsRight(a));
        CHECK(std::abs(crossings(a) - 33) <= 3); // 2 per ms over 16.7 ms
    }
    {
        Input in(fileConfig(fixture("f25_169.mkv")), {}, SessionClock::manual());
        const auto a = audioAt(in, samplesBefore(60), 801);
        float l = 0, r = 0;
        for (size_t i = 0; i < a.size(); i += 2) {
            l = std::max(l, std::fabs(a[i]));
            r = std::max(r, std::fabs(a[i + 1]));
        }
        CHECK(l > 0.05f);
        CHECK(r > 0.05f);
    }
}

TEST_CASE("looping restarts the picture and the sound") {
    {
        // f30 is 150 frames (5.005 s = tick 300), so the second pass starts at tick 300.
        Input in(fileConfig(fixture("f30.mkv")), {}, SessionClock::manual());
        for (int k : {0, 1, 2, 45}) {
            INFO("k " << k);
            CHECK(frameIndexOf(videoAtTick(in, 300 + 2 * k)) == k % 40);
        }
        const auto a = audioAt(in, 5200 * 48, 801); // 5.2 s: 0.195 s into the second pass
        CHECK(peak(a) > 0.1f); // lavfi sine: amplitude 1/8
        CHECK(std::abs(crossings(a) - 33) <= 3);
        CHECK(in.status().state == State::Running);
    }
    {
        Input in(fileConfig(fixture("f30.mkv"), false), {}, SessionClock::manual());
        CHECK(frameIndexOf(videoAtTick(in, 400)) == 149 % 40);
        CHECK(peak(audioAt(in, samplesBefore(400), 801)) == 0.0f);
        CHECK(in.status().state == State::Ended);
    }
}

TEST_CASE("missing, truncated and wrong files fail cleanly") {
    QuietLog quiet;
    {
        const std::string path = fixture("does_not_exist.mkv");
        Input in(fileConfig(path), {}, SessionClock::manual());
        vt::Frame f;
        CHECK_FALSE(in.videoAt(tickTime(0), f, Wait::Block));
        CHECK(peak(audioAt(in, 0, 801)) == 0.0f);
        const InputStatus st = in.status();
        CHECK(st.state == State::Error);
        CHECK(st.message.find("does_not_exist.mkv") != std::string::npos);
    }
    {
        // The first 20000 bytes of f30.mkv: a few whole frames, then the cut.
        TempPath cut("vt_input_truncated.mkv");
        {
            std::ifstream src(fixture("f30.mkv"), std::ios::binary);
            std::vector<char> head(20000);
            src.read(head.data(), std::streamsize(head.size()));
            std::ofstream(cut.path, std::ios::binary).write(head.data(), std::streamsize(head.size()));
        }
        Input in(fileConfig(cut.str(), false), {}, SessionClock::manual());
        std::vector<int> shown;
        for (int k = 0; k < 12; ++k) shown.push_back(frameIndexOf(videoAtTick(in, 2 * k)));
        const int last = shown.back();
        INFO("last frame before the cut " << last);
        CHECK(last >= 1);
        for (int k = 0; k < 12; ++k) CHECK(shown[size_t(k)] == std::min(k, last));
        CHECK(frameIndexOf(videoAtTick(in, 200)) == last); // holds
        CHECK(in.status().state == State::Ended);
    }
    {
        TempPath text("vt_input_not_video.mkv");
        std::ofstream(text.path) << "This is a text file, not a Matroska file.\n";
        Input in(fileConfig(text.str()), {}, SessionClock::manual());
        vt::Frame f;
        CHECK_FALSE(in.videoAt(tickTime(0), f, Wait::Block));
        CHECK(in.status().state == State::Error);
    }
}

TEST_CASE("a missing file that appears is picked up on retry") {
    QuietLog quiet;
    TempPath later("vt_input_appears.mkv");
    TempPath staging("vt_input_appears.part");
    InputConfig cfg = fileConfig(later.str());
    cfg.retryMs = 100;
    Input in(cfg, {}, SessionClock::manual());
    REQUIRE(waitForState(in, State::Error, std::chrono::seconds(2)));
    fs::copy_file(fixture("f30.mkv"), staging.path);
    fs::rename(staging.path, later.path); // appears whole
    CHECK(waitForState(in, State::Running, std::chrono::seconds(2)));
    CHECK(frameIndexOf(videoAtTick(in, 0)) == 0);
}

TEST_CASE("inputs without a source yet") {
    vt::Frame f;
    std::vector<float> a(1602, 1.0f);
    {
        Input in(InputConfig{}, {}, SessionClock::manual());
        CHECK_FALSE(in.videoAt(0, f, Wait::Block));
        in.audioAt(0, a, Wait::Block);
        CHECK(peak(a) == 0.0f);
        CHECK(in.status().state == State::Idle);
    }
    for (InputKind kind : {InputKind::Device, InputKind::Screen}) {
        InputConfig cfg;
        cfg.kind = kind;
        Input in(cfg, {}, SessionClock::manual());
        CHECK_FALSE(in.videoAt(0, f, Wait::Block));
        CHECK(in.status().state == State::Error);
        CHECK(in.status().message == "not supported yet");
    }
}

TEST_CASE("an input stops promptly with a full queue or in a retry sleep") {
    QuietLog quiet;
    using Clock = std::chrono::steady_clock;
    {
        auto in = std::make_unique<Input>(fileConfig(fixture("f30.mkv")), MediaGeometry{}, SessionClock::manual());
        // Nobody reads, so the thread fills its look-ahead and waits for room.
        const auto until = Clock::now() + std::chrono::seconds(5);
        while (in->status().framesDecoded < 16 && Clock::now() < until)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        CHECK(in->status().framesDecoded < 40); // it stopped ahead instead of decoding the file
        const auto t0 = Clock::now();
        in.reset();
        CHECK(Clock::now() - t0 < std::chrono::milliseconds(500));
    }
    {
        InputConfig cfg = fileConfig(fixture("does_not_exist.mkv"));
        cfg.retryMs = 60000;
        auto in = std::make_unique<Input>(cfg, MediaGeometry{}, SessionClock::manual());
        REQUIRE(waitForState(*in, State::Error, std::chrono::seconds(2)));
        const auto t0 = Clock::now();
        in.reset();
        CHECK(Clock::now() - t0 < std::chrono::milliseconds(500));
    }
}

TEST_CASE("videoAt copies a picture only when it changes") {
    Input in(fileConfig(fixture("f30.mkv")), {}, SessionClock::manual());
    vt::Frame out;
    REQUIRE(in.videoAt(tickTime(0), out, Wait::Block));
    out.at(1, 0) = {1, 2, 3, 4}; // a marker in the black margin, off the sampled grid
    REQUIRE(in.videoAt(tickTime(1), out, Wait::Block)); // still frame 0
    CHECK(out.at(1, 0) == vt::Rgba8{1, 2, 3, 4});
    REQUIRE(in.videoAt(tickTime(2), out, Wait::Block)); // frame 1
    CHECK(out.at(1, 0) == vt::Rgba8{0, 0, 0, 255});
    CHECK(frameIndexOf(out) == 1);
}

TEST_CASE("a damaged packet mid-file is skipped and counted") {
    QuietLog quiet;
    TempPath bad("vt_input_damaged.mkv");
    fs::copy_file(fixture("f30.mkv"), bad.path);
    {
        // Junk over the payload of frame 3 (its packet data starts at byte 13033), framing intact.
        std::fstream f(bad.path, std::ios::binary | std::ios::in | std::ios::out);
        f.seekp(13033 + 16);
        const std::vector<char> junk(600, '\xA7');
        f.write(junk.data(), std::streamsize(junk.size()));
    }
    Input in(fileConfig(bad.str(), false), {}, SessionClock::manual());
    CHECK(frameIndexOf(videoAtTick(in, 4)) == 2);
    CHECK(frameIndexOf(videoAtTick(in, 6)) == 2); // frame 3 is gone: frame 2 holds
    CHECK(frameIndexOf(videoAtTick(in, 2 * 20)) == 20);
    CHECK(frameIndexOf(videoAtTick(in, 2 * 149)) == 149 % 40);
    CHECK(frameIndexOf(videoAtTick(in, 400)) == 149 % 40);
    const InputStatus st = in.status();
    CHECK(st.state == State::Ended);
    CHECK(st.decodeErrors >= 1);
    CHECK(st.framesDecoded < 150);
}
