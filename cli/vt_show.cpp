// vt_show: runs a Switcher show over up to four inputs and records the Program output to a file.
// Offline (default) it renders as fast as the inputs decode, scripted by --auto-at / --take-at;
// with --realtime it runs against the clock (Ctrl+C stops it) and prints the inputs' and the
// loop's state once a second, which makes it the manual test bench for live devices.
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#define NOMINMAX
#include <windows.h>

#include "vt/effect.h"
#include "vt/media/devices.h"
#include "vt/media/program_loop.h"
#include "vt/media/recorder.h"
#include "vt/switcher.h"
#include "vt/timing.h"

namespace fs = std::filesystem;
using namespace vt::media;

namespace {

void usage(std::ostream& o) {
    o << "usage: vt_show --in1 <spec> [--in2 <spec>] [--in3 <spec>] [--in4 <spec>] --out <file>\n"
         "               [--format h264|mp4|ffv1|prores]\n"
         "               [--effect <crouton>|dissolve|cut] [--speed slow|medium|fast|variable]\n"
         "               [--auto-at <s>]... [--take-at <s>]... [--main <src>] [--preview <src>]\n"
         "               [--seconds <s>] [--realtime]\n"
         "       vt_show --list-devices\n"
         "\n"
         "  <spec>  file:<path> | dshow:<video name>[|<audio name>] | screen[:<window title>]\n"
         "          (dshow: either part may be empty: dshow:|Microphone for sound only)\n"
         "  <src>   in1..in4 | dv1 | dv2 | bkg\n"
         "\n"
         "Main defaults to in1, Preview to in2, the effect to dissolve, the speed to the effect's own,\n"
         "the format to h264 (MKV). --auto-at runs an Auto transition and --take-at a Take at the\n"
         "given second of the show. Offline needs --seconds; the file holds exactly\n"
         "round(seconds x 60000/1001) frames and none are dropped. --realtime runs against the clock\n"
         "until --seconds or Ctrl+C (or a recording error), and may drop frames if the encoder falls\n"
         "behind.\n"
         "\n"
         "Exit codes: 0 done; 1 the effect, the recording or the run failed; 2 bad arguments;\n"
         "3 offline, the file was written but an input had failed (black and silent; each is named).\n";
}

// ---- command line ---------------------------------------------------------------------------

struct Options {
    std::array<std::string, 4> in;
    std::string out, format = "h264", effect = "dissolve", speed, main = "in1", preview = "in2";
    std::vector<double> autoAt, takeAt;
    std::optional<double> seconds;
    bool realtime = false, listDevices = false;
};

// An error already printed means: exit with this code.
std::optional<int> parseArgs(const std::vector<std::string>& args, Options& o) {
    auto fail = [](const std::string& m) { std::cerr << "vt_show: " << m << "\n"; return std::optional<int>(2); };
    auto number = [&](const std::string& opt, const std::string& v, double& d) {
        char* end = nullptr;
        d = std::strtod(v.c_str(), &end);
        if (v.empty() || *end != '\0' || !std::isfinite(d) || d < 0) {
            fail(opt + " needs a number of seconds, not '" + v + "'");
            return false;
        }
        return true;
    };
    for (size_t i = 1; i < args.size(); ++i) {
        const std::string& a = args[i];
        if (a == "--help" || a == "-h") { usage(std::cout); return 0; }
        if (a == "--realtime") { o.realtime = true; continue; }
        if (a == "--list-devices") { o.listDevices = true; continue; }
        if (a.rfind("--", 0) != 0) { usage(std::cerr); return fail("unexpected argument '" + a + "'"); }
        if (i + 1 >= args.size()) return fail(a + " needs a value");
        const std::string& v = args[++i];
        double d = 0;
        if (a == "--in1") o.in[0] = v;
        else if (a == "--in2") o.in[1] = v;
        else if (a == "--in3") o.in[2] = v;
        else if (a == "--in4") o.in[3] = v;
        else if (a == "--out") o.out = v;
        else if (a == "--format") o.format = v;
        else if (a == "--effect") o.effect = v;
        else if (a == "--speed") o.speed = v;
        else if (a == "--main") o.main = v;
        else if (a == "--preview") o.preview = v;
        else if (a == "--auto-at") { if (!number(a, v, d)) return 2; o.autoAt.push_back(d); }
        else if (a == "--take-at") { if (!number(a, v, d)) return 2; o.takeAt.push_back(d); }
        else if (a == "--seconds") {
            if (!number(a, v, d)) return 2;
            if (d <= 0) return fail("--seconds must be positive");
            o.seconds = d;
        }
        else { usage(std::cerr); return fail("unknown option " + a); }
    }
    return std::nullopt;
}

fs::path pathFromUtf8(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }

// Fills `cfg` from "file:<path>", "dshow:<video>[|<audio>]" or "screen[:<window>]"; false with a
// reason in *why otherwise.
bool parseSpec(const std::string& spec, InputConfig& cfg, std::string* why) {
    auto starts = [&](const char* p) { return spec.rfind(p, 0) == 0; };
    if (starts("file:")) {
        cfg.kind = InputKind::File;
        cfg.path = spec.substr(5);
        std::error_code ec;
        if (cfg.path.empty() || !fs::exists(pathFromUtf8(cfg.path), ec)) { *why = "no such file '" + cfg.path + "'"; return false; }
        return true;
    }
    if (starts("dshow:")) {
        const std::string rest = spec.substr(6);
        const size_t bar = rest.find('|');
        cfg.kind = InputKind::Device;
        cfg.videoDevice = rest.substr(0, bar);
        if (bar != std::string::npos) cfg.audioDevice = rest.substr(bar + 1);
        if (cfg.videoDevice.empty() && cfg.audioDevice.empty()) { *why = "dshow: needs a video or an audio device name"; return false; }
        return true;
    }
    if (spec == "screen" || starts("screen:")) {
        cfg.kind = InputKind::Screen;
        if (spec.size() > 6) cfg.window = spec.substr(7);
        return true;
    }
    *why = "'" + spec + "' is not file:<path>, dshow:<video>[|<audio>] or screen[:<window>]";
    return false;
}

bool parseSource(const std::string& s, vt::Source& out) {
    static const std::pair<const char*, vt::Source> names[] = {
        {"in1", vt::Source::In1}, {"in2", vt::Source::In2}, {"in3", vt::Source::In3}, {"in4", vt::Source::In4},
        {"dv1", vt::Source::DV1}, {"dv2", vt::Source::DV2}, {"bkg", vt::Source::Bkg}};
    for (const auto& [n, v] : names)
        if (s == n) { out = v; return true; }
    return false;
}

bool parseFormat(const std::string& s, RecordFormat& out) {
    if (s == "h264") out = RecordFormat::H264Mkv;
    else if (s == "mp4") out = RecordFormat::H264Mp4;
    else if (s == "ffv1") out = RecordFormat::Ffv1Mkv;
    else if (s == "prores") out = RecordFormat::ProResMov;
    else return false;
    return true;
}

bool parseSpeed(const std::string& s, vt::Speed& out) {
    if (s == "slow") out = vt::Speed::Slow;
    else if (s == "medium") out = vt::Speed::Medium;
    else if (s == "fast") out = vt::Speed::Fast;
    else if (s == "variable") out = vt::Speed::Variable;
    else return false;
    return true;
}

int64_t secondsToTicks(double s) { return int64_t(std::llround(s * 60000.0 / 1001.0)); }

// ---- running --------------------------------------------------------------------------------

std::atomic<bool> g_stop{false};

BOOL WINAPI onCtrl(DWORD type) {
    if (type != CTRL_C_EVENT && type != CTRL_BREAK_EVENT && type != CTRL_CLOSE_EVENT) return FALSE;
    g_stop = true;
    // CTRL_CLOSE_EVENT ends the process once this returns; the main thread gets a moment to finish the file.
    if (type == CTRL_CLOSE_EVENT) std::this_thread::sleep_for(std::chrono::seconds(4));
    return TRUE;
}

struct Event { int64_t tick; bool autoTransition; };

const char* stateName(const ProgramLoop& loop, int i, InputKind kind) {
    if (kind == InputKind::None) return "-";
    switch (loop.inputStatus(i).state) {
        case InputStatus::State::Idle: return "idle";
        case InputStatus::State::Opening: return "opening";
        case InputStatus::State::Running: return "running";
        case InputStatus::State::Ended: return "ended";
        case InputStatus::State::Error: return "ERROR";
    }
    return "?";
}

void printStatus(const ProgramLoop& loop, const std::array<InputConfig, 4>& inputs, const Recorder& rec, int seconds) {
    const LoopStats st = loop.stats();
    const RecorderStatus rs = rec.status();
    std::printf("[%3ds]", seconds);
    for (int i = 0; i < 4; ++i) std::printf(" in%d:%s", i + 1, stateName(loop, i, inputs[size_t(i)].kind));
    std::printf(" | ticks %lld late %lld stalls %lld maxTick %.1fms | rec %lld frames %lld dropped\n",
                (long long)st.ticks, (long long)st.lateTicks, (long long)st.stalls, st.maxTickMs,
                (long long)rs.framesWritten, (long long)rs.framesDropped);
    for (int i = 0; i < 4; ++i) {
        const InputStatus s = loop.inputStatus(i);
        if (inputs[size_t(i)].kind != InputKind::None && s.state == InputStatus::State::Error)
            std::printf("       in%d: %s\n", i + 1, s.message.c_str());
    }
    std::fflush(stdout);
}

int listDevicesMain() {
    for (DeviceKind kind : {DeviceKind::Video, DeviceKind::Audio}) {
        std::string error;
        const auto devices = listDevices(kind, &error);
        std::printf("%s devices:\n", kind == DeviceKind::Video ? "Video" : "Audio");
        for (const DeviceInfo& d : devices) std::printf("  %s\n    id: %s\n", d.name.c_str(), d.id.c_str());
        if (devices.empty()) std::printf("  (none)\n");
        if (!error.empty()) std::fprintf(stderr, "vt_show: listing %s devices: %s\n", kind == DeviceKind::Video ? "video" : "audio", error.c_str());
    }
    return 0;
}

int run(const std::vector<std::string>& args) {
    Options o;
    if (const auto rc = parseArgs(args, o)) return *rc;
    if (o.listDevices) return listDevicesMain();

    auto bad = [](const std::string& m) { std::cerr << "vt_show: " << m << "\n"; return 2; };
    if (o.out.empty()) { usage(std::cerr); return 2; }
    ProgramConfig cfg;
    for (size_t i = 0; i < 4; ++i) {
        if (o.in[i].empty()) continue;
        std::string why;
        if (!parseSpec(o.in[i], cfg.inputs[i], &why)) return bad("--in" + std::to_string(i + 1) + ": " + why);
    }
    RecorderSettings rset;
    if (!parseFormat(o.format, rset.format)) return bad("unknown format '" + o.format + "' (h264, mp4, ffv1 or prores)");
    std::optional<vt::Speed> speed;
    if (!o.speed.empty()) {
        vt::Speed sp{};
        if (!parseSpeed(o.speed, sp)) return bad("unknown speed '" + o.speed + "'");
        speed = sp;
    }
    vt::Source mainSrc{}, previewSrc{};
    if (!parseSource(o.main, mainSrc)) return bad("unknown source '" + o.main + "' for --main");
    if (!parseSource(o.preview, previewSrc)) return bad("unknown source '" + o.preview + "' for --preview");
    if (!o.realtime && !o.seconds) return bad("offline needs --seconds (or use --realtime)");

    std::shared_ptr<const vt::Effect> effect;
    if (o.effect == "dissolve") effect = vt::makeDissolve();
    else if (o.effect == "cut") effect = vt::makeCut();
    else {
        std::string why;
        effect = vt::loadEffect(pathFromUtf8(o.effect), &why);
        if (!effect) {
            std::cerr << "vt_show: cannot load effect " << o.effect << ": " << (why.empty() ? "unknown reason" : why) << "\n";
            return 1;
        }
    }

    std::vector<Event> events;
    for (double s : o.autoAt) events.push_back({secondsToTicks(s), true});
    for (double s : o.takeAt) events.push_back({secondsToTicks(s), false});
    std::stable_sort(events.begin(), events.end(), [](const Event& a, const Event& b) { return a.tick < b.tick; });
    const int64_t total = o.seconds ? secondsToTicks(*o.seconds) : INT64_MAX;
    for (const Event& e : events)
        if (e.tick >= total) std::cerr << "vt_show: warning: an event at tick " << e.tick << " is past the end of the show and will not run\n";

    rset.path = pathFromUtf8(o.out);
    rset.geometry = cfg.geometry;
    rset.blockWhenFull = !o.realtime;   // offline: no dropped frames (Ruling 4)

    Recorder rec;          // outlives the loop, which holds it as a sink
    ProgramLoop loop(cfg);
    std::string error;
    if (!rec.start(rset, &error)) {
        std::cerr << "vt_show: cannot record to " << o.out << ": " << error << "\n";
        return 1;
    }
    loop.addSink(&rec);

    auto effectRefused = std::make_shared<std::atomic<bool>>(false);
    loop.post([=](vt::Switcher& sw) {
        std::string why;
        if (!sw.setEffect(effect, &why)) {   // also sets the speed button to the effect's own
            std::cerr << "vt_show: effect refused: " << why << "\n";
            *effectRefused = true;
        }
        if (speed) sw.setSpeed(*speed);
        sw.select(vt::Bus::Main, mainSrc);
        sw.select(vt::Bus::Preview, previewSrc);
    });
    auto fire = [&](const Event& e) {
        if (e.autoTransition) loop.post([=](vt::Switcher& sw) { sw.autoTransition(speed.value_or(sw.speed())); });
        else loop.post([](vt::Switcher& sw) { sw.take(); });
    };

    if (!o.realtime) {
        // Posted commands run at the start of the next tick, so posting once `e.tick` ticks have run
        // makes the command act on tick e.tick.
        int64_t done = 0;
        for (const Event& e : events) {
            if (e.tick >= total) break;
            if (e.tick > done) { loop.runOffline(e.tick - done); done = e.tick; }
            fire(e);
        }
        loop.runOffline(total - done);
    } else {
        SetConsoleCtrlHandler(onCtrl, TRUE);
        loop.start();
        size_t next = 0;
        const auto began = std::chrono::steady_clock::now();
        int printed = 0;
        bool recFailed = false;
        while (!g_stop && loop.stats().ticks < total) {
            if (!rec.status().error.empty()) { recFailed = true; break; } // nothing more is recorded
            const int64_t ticks = loop.stats().ticks;
            while (next < events.size() && events[next].tick <= ticks) fire(events[next++]);
            const int elapsed = int(std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - began).count());
            if (elapsed > printed) { printed = elapsed; printStatus(loop, cfg.inputs, rec, elapsed); }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        loop.stop();
        rec.stop();
        std::printf("%s\n", recFailed || g_stop ? "stopped" : "done");
        printStatus(loop, cfg.inputs, rec, printed);
    }
    rec.stop();   // offline, or again after an error (no-op)

    const RecorderStatus rs = rec.status();
    if (*effectRefused) return 1;
    if (!rs.error.empty()) {
        std::cerr << "vt_show: recording failed: " << rs.error << "\n";
        return 1;
    }
    std::printf("wrote %lld frames to %s (%lld dropped)\n", (long long)rs.framesWritten, o.out.c_str(), (long long)rs.framesDropped);
    if (!o.realtime) {
        // Offline a failed input is black and silent in the file: say which, and exit 3.
        bool inputFailed = false;
        for (int i = 0; i < 4; ++i) {
            const InputStatus s = loop.inputStatus(i);
            if (cfg.inputs[size_t(i)].kind == InputKind::None || s.state != InputStatus::State::Error) continue;
            std::cerr << "vt_show: warning: in" << i + 1 << " failed: " << s.message << "\n";
            inputFailed = true;
        }
        if (inputFailed) return 3;
    }
    return 0;
}

std::string toUtf8(const wchar_t* w) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(size_t(std::max(n, 1)) - 1, '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

}  // namespace

// wmain: device and window names and file paths arrive as UTF-8, which is what FFmpeg expects.
int wmain(int argc, wchar_t** argv) {
    std::vector<std::string> args;
    for (int i = 0; i < argc; ++i) args.push_back(toUtf8(argv[i]));
    try {
        return run(args);
    } catch (const std::exception& e) {
        std::cerr << "vt_show: " << e.what() << "\n";
        return 1;
    }
}
