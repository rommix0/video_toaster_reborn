#include "catch_amalgamated.hpp"
#include "vt/background.h"
#include "vt/effect.h"
#include "vt/log.h"
#include "vt/media/program_loop.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <string>
#include <thread>
#include <vector>

using namespace vt::media;
namespace fs = std::filesystem;
using State = InputStatus::State;

namespace {

std::string fixture(const char* name) { return (fs::path(VT_MEDIA_FIXTURES) / name).string(); }

InputConfig fileConfig(const std::string& path) {
    InputConfig c;
    c.kind = InputKind::File;
    c.path = path;
    return c;
}

// The fixtures' luma code: Y = 16 + 5 * (N mod 40), read back from the raster centre's RGB.
int frameIndexOf(vt::Rgba8 centre) { return int(std::lround(centre.r * 219.0 / 1275.0)); }

bool isBlack(const vt::Frame& f) {
    return std::all_of(f.px.begin(), f.px.end(), [](vt::Rgba8 p) { return p.r == 0 && p.g == 0 && p.b == 0; });
}

// Keeps what the loop delivers: tick numbers, the centre pixel, the audio, and whole pictures
// for the ticks `keepFrame` picks.
struct Recorder : ProgramSink {
    std::vector<int64_t> ticks;
    std::vector<vt::Rgba8> centre;
    std::vector<std::vector<float>> audio;
    std::map<int64_t, vt::Frame> frames;
    std::function<bool(int64_t)> keepFrame;
    std::function<void(int64_t)> onTick;

    void onProgram(int64_t tick, const vt::Frame& program, std::span<const float> stereo) override {
        ticks.push_back(tick);
        centre.push_back(program.at(384, 241));
        audio.emplace_back(stereo.begin(), stereo.end());
        if (keepFrame && keepFrame(tick)) frames[tick] = program;
        if (onTick) onTick(tick);
    }
    // The audio of ticks [first, last) in one block.
    std::vector<float> audioOf(int64_t first, int64_t last) const {
        std::vector<float> v;
        for (int64_t n = first; n < last; ++n) v.insert(v.end(), audio[size_t(n)].begin(), audio[size_t(n)].end());
        return v;
    }
};

double rms(const std::vector<float>& v) {
    double s = 0;
    for (float x : v) s += double(x) * x;
    return v.empty() ? 0 : std::sqrt(s / double(v.size()));
}

float peak(const std::vector<float>& v) {
    float p = 0;
    for (float x : v) p = std::max(p, std::fabs(x));
    return p;
}

// Sign changes of the left channel per stereo frame.
double crossingRate(const std::vector<float>& v) {
    int n = 0;
    for (size_t i = 1; i < v.size() / 2; ++i) n += (v[(i - 1) * 2] < 0) != (v[i * 2] < 0);
    return double(n) / double(v.size() / 2);
}

// A file's audio for ticks [first, last), read on its own (the loop's inputs open at tick 0).
std::vector<float> fileAudio(const std::string& path, int64_t first, int64_t last) {
    Input in(fileConfig(path), {}, SessionClock::manual());
    std::vector<float> v(size_t(samplesBefore(last) - samplesBefore(first)) * 2);
    in.audioAt(samplesBefore(first), v, Wait::Block);
    return v;
}

int fastTicks(const vt::Effect& e) {
    const vt::Timing t = e.timing();
    return t.durationTicks[static_cast<size_t>(t.resolve(vt::Speed::Fast))];
}

// One CSV line split into fields ("" quotes, no line breaks inside fields).
std::vector<std::string> csvFields(const std::string& line) {
    std::vector<std::string> out(1);
    bool quoted = false;
    for (size_t i = 0; i < line.size(); ++i) {
        const char c = line[i];
        if (quoted && c == '"' && i + 1 < line.size() && line[i + 1] == '"') out.back() += line[++i];
        else if (c == '"') quoted = !quoted;
        else if (c == ',' && !quoted) out.emplace_back();
        else out.back() += c;
    }
    return out;
}

// The first converted/manifest.csv row with a non-empty `sound` column (Plan 1 Ruling 4): the
// source path under the data root.
fs::path firstEffectWithSound() {
    const fs::path root = fs::path(VT_DATA_ROOT).lexically_normal();
    std::ifstream f(root / "converted" / "manifest.csv");
    REQUIRE(f);
    std::string line;
    std::getline(f, line);
    const auto head = csvFields(line);
    const auto col = [&](const char* name) { return size_t(std::find(head.begin(), head.end(), name) - head.begin()); };
    const size_t source = col("source"), sound = col("sound");
    REQUIRE(sound < head.size());
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const auto row = csvFields(line);
        if (row.size() > sound && !row[sound].empty()) return root / fs::path(row[source]);
    }
    FAIL("no manifest row has a sound");
    return {};
}

// Captures warnings (the missing file's complaint) so test output stays clean.
struct QuietLog {
    std::vector<std::string> lines;
    QuietLog() { vt::setLogSink([this](std::string_view m) { lines.emplace_back(m); }); }
    ~QuietLog() { vt::setLogSink({}); }
};

} // namespace

TEST_CASE("offline: Program shows In1, then dissolves to In2 over durationTicks") {
    ProgramConfig cfg;
    cfg.inputs[0] = fileConfig(fixture("f30.mkv"));
    cfg.inputs[1] = fileConfig(fixture("f25_169.mkv"));
    ProgramLoop loop(cfg);
    Recorder rec;
    int dur = 0;
    rec.keepFrame = [&](int64_t n) { return dur > 0 && n >= 30 + dur; };
    loop.addSink(&rec);

    loop.runOffline(30);
    loop.post([&](vt::Switcher& sw) {
        sw.setEffect(vt::makeDissolve());
        sw.autoTransition(vt::Speed::Fast);
        dur = fastTicks(sw.effect());
    });
    loop.runOffline(1);
    REQUIRE(dur > 0);
    loop.runOffline(dur + 9);
    REQUIRE(rec.ticks.size() == size_t(40 + dur));

    for (int64_t n = 0; n <= 30; ++n) { // tick 30 is the Auto's first frame: T-bar 0, still In1
        INFO("tick " << n);
        CHECK(rec.ticks[size_t(n)] == n);
        CHECK(frameIndexOf(rec.centre[size_t(n)]) == (n / 2) % 40);
    }
    Input in2(fileConfig(fixture("f25_169.mkv")), {}, SessionClock::manual());
    vt::Frame want;
    for (int64_t n = 30 + dur; n < 40 + dur; ++n) {
        INFO("tick " << n);
        REQUIRE(in2.videoAt(tickTime(n), want, Wait::Block));
        REQUIRE(rec.frames.count(n));
        CHECK(rec.frames[n].px == want.px);
    }
}

TEST_CASE("every tick delivers its exact sample count") {
    ProgramConfig cfg;
    cfg.inputs[0] = fileConfig(fixture("f30.mkv"));
    ProgramLoop loop(cfg);
    Recorder rec;
    loop.addSink(&rec);
    loop.runOffline(600);
    REQUIRE(rec.ticks.size() == 600);
    int64_t total = 0;
    for (size_t n = 0; n < rec.audio.size(); ++n) {
        CHECK(rec.ticks[n] == int64_t(n));
        CHECK(rec.audio[n].size() == size_t(samplesInTick(int64_t(n))) * 2);
        total += int64_t(rec.audio[n].size() / 2);
    }
    CHECK(total == samplesBefore(600));
    CHECK(total == 480480);
    CHECK(loop.stats().ticks == 600);
}

TEST_CASE("audio crossfades with the dissolve") {
    ProgramConfig cfg;
    cfg.inputs[0] = fileConfig(fixture("f30.mkv"));
    cfg.inputs[1] = fileConfig(fixture("f25_169.mkv"));
    ProgramLoop loop(cfg);
    Recorder rec;
    loop.addSink(&rec);
    int dur = 0;
    loop.runOffline(30);
    loop.post([&](vt::Switcher& sw) {
        sw.setEffect(vt::makeDissolve());
        sw.autoTransition(vt::Speed::Fast);
        dur = fastTicks(sw.effect());
    });
    loop.runOffline(1);
    REQUIRE(dur > 0);
    loop.runOffline(dur + 30);

    const std::string f30 = fixture("f30.mkv"), f25 = fixture("f25_169.mkv");
    // Before: In1 alone (1 kHz: 2 sign changes per ms, 1/24 per frame).
    const auto before = rec.audioOf(10, 30);
    CHECK(rms(before) == Catch::Approx(rms(fileAudio(f30, 10, 30))).epsilon(0.02));
    CHECK(crossingRate(before) == Catch::Approx(2000.0 / 48000).epsilon(0.1));
    // After: In2 alone (440 Hz).
    const int64_t a0 = 30 + dur + 2, a1 = a0 + 20;
    const auto after = rec.audioOf(a0, a1);
    CHECK(rms(after) == Catch::Approx(rms(fileAudio(f25, a0, a1))).epsilon(0.02));
    CHECK(crossingRate(after) == Catch::Approx(880.0 / 48000).epsilon(0.1));
    // Middle (T-bar ~255): cos = sin = 0.707, equal power: rms^2 = (r1^2 + r2^2) / 2.
    const int64_t m = 30 + dur / 2;
    const double r1 = rms(fileAudio(f30, m, m + 1)), r2 = rms(fileAudio(f25, m, m + 1));
    CHECK(rms(rec.audio[size_t(m)]) == Catch::Approx(std::sqrt((r1 * r1 + r2 * r2) / 2)).epsilon(0.1));
}

TEST_CASE("posted commands apply at the next tick, in order") {
    ProgramLoop loop({});
    Recorder rec;
    std::vector<int> order;
    rec.onTick = [&](int64_t n) {
        if (n == 5) loop.post([](vt::Switcher& sw) { sw.setBackground(vt::BkgColour::Green); });
    };
    loop.addSink(&rec);
    loop.post([&](vt::Switcher& sw) {
        sw.select(vt::Bus::Main, vt::Source::Bkg);
        order.push_back(1);
    });
    loop.post([&](vt::Switcher& sw) {
        sw.setBackground(vt::BkgColour::Red);
        order.push_back(2);
    });
    loop.post([&](vt::Switcher& sw) {
        sw.setBackground(vt::BkgColour::Blue);
        order.push_back(3);
    });
    loop.runOffline(10);
    CHECK(order == std::vector<int>{1, 2, 3});
    REQUIRE(rec.centre.size() == 10);
    for (size_t n = 0; n < 10; ++n) {
        INFO("tick " << n);
        CHECK(rec.centre[n] == vt::bkgColour(n <= 5 ? vt::BkgColour::Blue : vt::BkgColour::Green));
    }
}

TEST_CASE("an input that fails keeps the Program running") {
    QuietLog quiet;
    ProgramConfig cfg;
    cfg.inputs[0] = fileConfig((fs::temp_directory_path() / "vt_no_such_input.mkv").string());
    cfg.inputs[1] = fileConfig(fixture("f30.mkv"));
    ProgramLoop loop(cfg);
    Recorder rec;
    rec.keepFrame = [](int64_t) { return true; };
    loop.addSink(&rec);
    loop.runOffline(120);
    REQUIRE(rec.ticks.size() == 120);
    for (int64_t n = 0; n < 120; ++n) {
        INFO("tick " << n);
        CHECK(isBlack(rec.frames[n])); // In1 is Main
        CHECK(peak(rec.audio[size_t(n)]) == 0.0f);
    }
    CHECK(loop.inputStatus(0).state == State::Error);
    CHECK(loop.inputStatus(1).state == State::Running);
}

TEST_CASE("setInput replaces an input at the next tick, starting from its first frame") {
    ProgramConfig cfg;
    cfg.inputs[0] = fileConfig(fixture("f30.mkv"));
    ProgramLoop loop(cfg);
    Recorder rec;
    loop.addSink(&rec);
    loop.runOffline(10);
    loop.setInput(0, fileConfig(fixture("f25_169.mkv")));
    CHECK(loop.inputStatus(0).width == 640); // still the old one until the next tick
    loop.runOffline(30);
    REQUIRE(rec.centre.size() == 40);
    for (int64_t n = 0; n < 40; ++n) {
        INFO("tick " << n);
        const int want = n < 10 ? int(n / 2 % 40) : int((n - 10) * 25025 / 60000 % 40); // Ruling 1
        CHECK(frameIndexOf(rec.centre[size_t(n)]) == want);
    }
    CHECK(loop.inputStatus(0).width == 1280);
}

TEST_CASE("effect sound reaches the sinks") {
    std::string why;
    std::shared_ptr<const vt::Effect> e = vt::loadEffect(firstEffectWithSound(), &why);
    INFO(why);
    REQUIRE(e);
    REQUIRE(e->sound());
    ProgramLoop loop({}); // no inputs: only the effect sound makes a sound
    Recorder rec;
    loop.addSink(&rec);
    loop.runOffline(10);
    bool accepted = false;
    loop.post([&](vt::Switcher& sw) {
        accepted = sw.setEffect(e);
        sw.autoTransition(vt::Speed::Medium);
    });
    loop.runOffline(10);
    REQUIRE(accepted);
    for (size_t n = 0; n < 10; ++n) CHECK(peak(rec.audio[n]) == 0.0f);
    CHECK(peak(rec.audio[10]) > 0.0f); // the Auto's first tick
    CHECK(peak(rec.audioOf(10, 20)) > 0.01f);
}

TEST_CASE("real time keeps 59.94 fps", "[live]") {
    ProgramConfig cfg;
    cfg.inputs[0] = fileConfig(fixture("f30.mkv"));
    ProgramLoop loop(cfg);
    Recorder rec;
    loop.addSink(&rec);
    loop.start();
    loop.start(); // idempotent
    std::this_thread::sleep_for(std::chrono::microseconds(2002000));
    const LoopStats st = loop.stats();
    loop.stop();
    loop.stop(); // idempotent
    INFO("ticks " << st.ticks << ", late " << st.lateTicks << ", stalls " << st.stalls << ", max " << st.maxTickMs << " ms");
    CHECK(std::abs(st.ticks - 120) <= 3);
    CHECK(st.lateTicks <= 3);
    CHECK(st.stalls == 0);
    for (size_t n = 0; n < rec.ticks.size(); ++n) CHECK(rec.ticks[n] == int64_t(n));
    CHECK(loop.inputStatus(0).state == State::Running);
}

TEST_CASE("real time re-bases after falling more than 1 s behind", "[live]") {
    ProgramConfig cfg;
    cfg.inputs[0] = fileConfig(fixture("f30.mkv"));
    ProgramLoop loop(cfg);
    Recorder rec;
    rec.onTick = [](int64_t n) {
        if (n == 10) std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    };
    loop.addSink(&rec);
    loop.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(1600));
    loop.stop();
    const LoopStats st = loop.stats();
    INFO("ticks " << st.ticks << ", late " << st.lateTicks << ", stalls " << st.stalls);
    CHECK(st.stalls == 1);
    CHECK(st.lateTicks <= 3);
    CHECK(st.maxTickMs >= 1100);
    // Ticks 0..10 (tick 10 ends at ~1.27 s), up to 5 ticks' wait for the re-based deadline,
    // then 59.94 per second until 1.6 s: 26..31 ticks, none skipped.
    CHECK(st.ticks >= 24);
    CHECK(st.ticks <= 33);
    for (size_t n = 0; n < rec.ticks.size(); ++n) CHECK(rec.ticks[n] == int64_t(n));
    for (size_t n = 0; n < rec.audio.size(); ++n) CHECK(rec.audio[n].size() == size_t(samplesInTick(int64_t(n))) * 2);
}
