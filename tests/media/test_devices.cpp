#include "catch_amalgamated.hpp"
#include "vt/log.h"
#include "vt/media/devices.h"
#include "vt/media/input.h"

#include <chrono>
#include <cmath>
#include <future>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

using namespace vt::media;
using State = InputStatus::State;
using Steady = std::chrono::steady_clock;

namespace {

bool waitForState(const Input& in, State want, std::chrono::milliseconds limit) {
    const auto until = Steady::now() + limit;
    while (in.status().state != want) {
        if (Steady::now() > until) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return true;
}

// Captures warnings (FFmpeg's complaints about missing devices, clock re-anchoring) so test
// output stays clean. The decode thread logs while the test reads, hence the lock.
struct QuietLog {
    mutable std::mutex mu;
    std::vector<std::string> lines;
    QuietLog() {
        vt::setLogSink([this](std::string_view m) {
            std::lock_guard l(mu);
            lines.emplace_back(m);
        });
    }
    ~QuietLog() { vt::setLogSink({}); }
    int count(std::string_view part) const {
        std::lock_guard l(mu);
        int n = 0;
        for (const auto& s : lines) n += s.find(part) != std::string::npos;
        return n;
    }
};

// A plain white top-level window with its own message loop, for window-capture tests.
class TestWindow {
public:
    explicit TestWindow(const std::string& title) {
        std::promise<HWND> made;
        auto madeF = made.get_future();
        thread_ = std::thread([made = std::move(made), title]() mutable {
            WNDCLASSW wc{};
            wc.lpfnWndProc = proc;
            wc.hInstance = GetModuleHandleW(nullptr);
            wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH));
            wc.lpszClassName = L"vtoast_test_window";
            RegisterClassW(&wc); // fails harmlessly if a previous test registered it
            const std::wstring wtitle(title.begin(), title.end()); // ASCII titles only
            HWND h = CreateWindowExW(0, wc.lpszClassName, wtitle.c_str(), WS_OVERLAPPEDWINDOW | WS_VISIBLE, 100, 100,
                                     320, 240, nullptr, nullptr, wc.hInstance, nullptr);
            made.set_value(h);
            if (!h) return;
            MSG m;
            while (GetMessageW(&m, nullptr, 0, 0) > 0) {
                TranslateMessage(&m);
                DispatchMessageW(&m);
            }
        });
        hwnd_ = madeF.get();
    }
    ~TestWindow() {
        close();
        thread_.join();
    }
    bool ok() const { return hwnd_ != nullptr; }
    void close() {
        if (hwnd_) PostMessageW(hwnd_, WM_CLOSE, 0, 0);
        hwnd_ = nullptr;
    }

private:
    static LRESULT CALLBACK proc(HWND h, UINT msg, WPARAM w, LPARAM l) {
        if (msg == WM_DESTROY) {
            PostQuitMessage(0);
            return 0;
        }
        return DefWindowProcW(h, msg, w, l);
    }
    std::thread thread_;
    HWND hwnd_ = nullptr;
};

} // namespace

TEST_CASE("device lists never throw") {
    for (DeviceKind kind : {DeviceKind::Video, DeviceKind::Audio}) {
        std::string e = "stale";
        const std::vector<DeviceInfo> v = listDevices(kind, &e);
        if (e.empty()) {
            for (const DeviceInfo& d : v) CHECK_FALSE(d.id.empty());
        } else {
            CHECK(v.empty());
        }
        CHECK_NOTHROW(listDevices(kind));
    }
}

TEST_CASE("a device that does not exist fails cleanly") {
    QuietLog quiet;
    InputConfig cfg;
    cfg.kind = InputKind::Device;
    cfg.videoDevice = "vtoast-no-such-camera";
    cfg.retryMs = 200;
    SessionClock clock = SessionClock::realtime();
    const auto t0 = Steady::now();
    {
        Input in(cfg, {}, clock);
        REQUIRE(waitForState(in, State::Error, std::chrono::seconds(3)));
        vt::Frame f;
        CHECK_FALSE(in.videoAt(clock.now(), f, Wait::Block));
        std::vector<float> a(1602, 1.0f);
        in.audioAt(clock.now() / 5, a, Wait::Block);
        for (float x : a) REQUIRE(x == 0.0f);
        // It keeps trying: every retryMs the device is looked for again.
        const auto until = Steady::now() + std::chrono::seconds(2);
        while (quiet.count("vtoast-no-such-camera") < 2 && Steady::now() < until)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        CHECK(quiet.count("vtoast-no-such-camera") >= 2);
    }
    CHECK(Steady::now() - t0 < std::chrono::seconds(5));
}

TEST_CASE("a device input needs a device dshow can name") {
    InputConfig cfg;
    cfg.kind = InputKind::Device;
    {
        Input in(cfg, {}, SessionClock::manual());
        CHECK(in.status().state == State::Error);
        vt::Frame f;
        CHECK_FALSE(in.videoAt(0, f, Wait::Block));
    }
    cfg.videoDevice = "Camera: Front"; // dshow splits its input string at ':', with no escape
    Input in(cfg, {}, SessionClock::manual());
    CHECK(in.status().state == State::Error);
    CHECK(in.status().message.find("Camera: Front") != std::string::npos);
}

TEST_CASE("desktop capture delivers frames", "[live]") {
    QuietLog quiet;
    InputConfig cfg;
    cfg.kind = InputKind::Screen;
    SessionClock clock = SessionClock::realtime();
    const MediaGeometry geo;
    Input in(cfg, geo, clock);
    REQUIRE(waitForState(in, State::Running, std::chrono::seconds(3)));

    // A live input never "ends": a blocking read gives up after a bounded wait.
    vt::Frame f;
    const auto t0 = Steady::now();
    in.videoAt(clock.now() + 240000 * 10, f, Wait::Block);
    CHECK(Steady::now() - t0 < std::chrono::milliseconds(500));

    bool got = false;
    const auto until = Steady::now() + std::chrono::seconds(3);
    while (!got && Steady::now() < until) {
        got = in.videoAt(clock.now(), f, Wait::No);
        if (!got) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    REQUIRE(got);
    const InputStatus st = in.status();
    REQUIRE(st.width > 0);
    REQUIRE(st.height > 0);
    CHECK_FALSE(st.hasAudio);
    const vt::Rect r = fitRect(st.width, st.height, {1, 1}, geo);
    CHECK(r.x >= geo.inputRect.x);
    CHECK(r.y >= geo.inputRect.y);
    CHECK(r.x + r.w <= geo.inputRect.x + geo.inputRect.w);
    CHECK(r.y + r.h <= geo.inputRect.y + geo.inputRect.h);
    int outsideNotBlack = 0, insideNotOpaque = 0;
    for (int y = 0; y < vt::kRasterH; ++y)
        for (int x = 0; x < vt::kRasterW; ++x) {
            const vt::Rgba8 p = f.at(x, y);
            if (x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h) insideNotOpaque += p.a != 255;
            else outsideNotBlack += !(p == vt::Rgba8{0, 0, 0, 255});
        }
    CHECK(outsideNotBlack == 0);
    CHECK(insideNotOpaque == 0);

    const uint64_t before = in.status().framesDecoded;
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    CHECK(in.status().framesDecoded > before);
    CHECK(in.status().state == State::Running);
}

TEST_CASE("first real camera opens", "[live]") {
    const std::vector<DeviceInfo> cams = listDevices(DeviceKind::Video);
    if (cams.empty()) SKIP("no camera");
    QuietLog quiet;
    InputConfig cfg;
    cfg.kind = InputKind::Device;
    cfg.videoDevice = cams.front().id;
    INFO(cams.front().name << " (" << cams.front().id << ")");
    Input in(cfg, {}, SessionClock::realtime());
    CHECK(waitForState(in, State::Running, std::chrono::seconds(5)));
}

TEST_CASE("a captured window that closes goes black", "[live]") {
    QuietLog quiet;
    const std::string title = "vtoast capture test " + std::to_string(GetCurrentProcessId());
    TestWindow win(title);
    REQUIRE(win.ok());
    InputConfig cfg;
    cfg.kind = InputKind::Screen;
    cfg.window = title;
    cfg.retryMs = 200;
    SessionClock clock = SessionClock::realtime();
    Input in(cfg, {}, clock);
    REQUIRE(waitForState(in, State::Running, std::chrono::seconds(3)));
    // The white window shows (the first grabs may predate its first paint).
    vt::Frame f;
    bool white = false;
    for (const auto until = Steady::now() + std::chrono::seconds(3); !white && Steady::now() < until;) {
        white = in.videoAt(clock.now(), f, Wait::No) && f.at(384, 241).r > 200;
        if (!white) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    REQUIRE(white);

    win.close();
    REQUIRE(waitForState(in, State::Error, std::chrono::seconds(5)));
    // Black from then on, while it keeps looking for the window every retryMs.
    int shown = 0;
    for (int i = 0; i < 20; ++i) {
        shown += in.videoAt(clock.now(), f, Wait::No);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    CHECK(shown == 0);
    CHECK(in.status().state != State::Running);
}
