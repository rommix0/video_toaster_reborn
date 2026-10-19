#include "catch_amalgamated.hpp"
#include "vt/media/geometry.h"
#include "vt/media/time.h"

#include <thread>
#include <vector>

using namespace vt::media;

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
TEST_CASE("clock copies share one origin") {
    auto m = SessionClock::manual();
    auto copy = m;
    m.set(99);
    CHECK(copy.now() == 99);

    auto r = SessionClock::realtime();
    auto rc = r;
    r.set(1000000000); // ignored on a realtime clock
    const SessionTime a = r.now();
    CHECK(a < 240000); // far below a second since construction
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    const SessionTime b = rc.now();
    CHECK(b >= a + 3000); // 20 ms is ~4800 units; allow slack
    CHECK(b < 240000);
}
TEST_CASE("fitRect keeps the display aspect") {
    MediaGeometry g;
    CHECK(fitRect(640, 480, {1, 1}, g) == vt::Rect{32, 1, 704, 480});
    CHECK(fitRect(704, 480, {10, 11}, g) == vt::Rect{32, 1, 704, 480});
    CHECK(fitRect(720, 480, {8, 9}, g) == vt::Rect{32, 1, 704, 480});
    CHECK(fitRect(1920, 1080, {1, 1}, g) == vt::Rect{32, 61, 704, 360});
    CHECK(fitRect(1080, 1920, {1, 1}, g) == vt::Rect{235, 1, 297, 480});
    CHECK(fitRect(1920, 1080, {0, 1}, g) == vt::Rect{32, 61, 704, 360});
}
