// The app end to end: AppController in Mode::Offline with the Plan 2 file fixtures as inputs, driven
// through the Switcher screen, the menus and the settings dialog.
#include "controller.h"

#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QElapsedTimer>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSlider>
#include <QRegularExpression>
#include <QStatusBar>
#include <QTemporaryDir>
#include <QTimer>
#include <QtTest>
#include <memory>
#include <thread>

#include "main_window.h"
#include "mixer_dock.h"
#include "monitor_window.h"
#include "position_dock.h"
#include "project_grid.h"
#include "test_support.h"
#include "toaster_screen.h"
#include "vt/effect.h"
#include "vt/framestore.h"
#include "vt/log.h"
#include "vt/media/devices.h"
#include "vt/media/ffmpeg.h"
#include "vt/media/program_loop.h"

using apptest::centre;
using apptest::node;
using Mode = AppController::Mode;

namespace {

namespace fs = std::filesystem;

vt::media::InputConfig fileInput(const char* name) {
    vt::media::InputConfig c;
    c.kind = vt::media::InputKind::File;
    c.path = (fs::path(VT_MEDIA_FIXTURES) / name).string();
    return c;
}

QString dataRoot() { return QDir::cleanPath(QStringLiteral(VT_DATA_ROOT)); }

// In1 = f30.mkv, In2 = f25_169.mkv; zoom 1 everywhere; recordings in `dir`.
AppSettings testSettings(const QTemporaryDir& dir) {
    AppSettings s(dir.filePath(QStringLiteral("vtoast.ini")));
    s.toasterRoot = dataRoot();
    s.inputs[0] = fileInput("f30.mkv");
    s.inputs[1] = fileInput("f25_169.mkv");
    s.recordingPath = dir.filePath(QStringLiteral("rec"));
    s.framestorePath = dir.filePath(QStringLiteral("Saved"));
    s.screenZoom = 1;
    s.monitorZoom = 1;
    return s;
}

// Shows a drawer and returns the index of the named crouton in it, scrolled into view.
int showCrouton(AppController& c, const char* drawer, const char* name) {
    QComboBox* picker = c.projectView()->drawerPicker();
    const int d = picker->findText(QString::fromLatin1(drawer));
    if (d < 0) return -1;
    picker->setCurrentIndex(d);
    const auto& croutons = c.screen()->project().croutons();
    for (size_t i = 0; i < croutons.size(); ++i)
        if (croutons[i].name == name) {
            c.screen()->setScrollRow(int(i) / ProjectGrid::kColumns);
            return int(i);
        }
    return -1;
}

// The centre of crouton i's cell on the screen at zoom 1.
QPoint cellCentre(const ToasterScreen& s, int i) {
    const int slot = i - s.project().scrollRow() * ProjectGrid::kColumns;
    return {ProjectGrid::kLeft + slot % ProjectGrid::kColumns * vt::assets::kCroutonW + vt::assets::kCroutonW / 2,
            ProjectGrid::kTop + slot / ProjectGrid::kColumns * vt::assets::kCroutonH + vt::assets::kCroutonH / 2};
}

int autoTicks(const fs::path& effectFile) {
    const auto e = vt::loadEffect(effectFile);
    if (!e) return -1;
    const vt::Timing t = e->timing();
    return t.durationTicks[size_t(t.resolve(t.defaultSpeed))];
}

// An audio output whose start fails `failures` times, then works; the test can make it go away.
struct FakeAudio : AudioDevice {
    int failures = 0, starts = 0;
    bool on = false;
    void onProgram(int64_t, const vt::Frame&, std::span<const float>) override {}
    bool start(const std::string&, std::string* why) override {
        ++starts;
        if (failures > 0) {
            --failures;
            *why = "no playback device";
            return false;
        }
        on = true;
        return true;
    }
    void stop() override { on = false; }
    bool running() const override { return on; }
};

struct LastPicture : vt::media::MonitorSink {
    vt::Frame program, preview;
    void onPictures(int64_t, const vt::Frame& p, const vt::Frame& v) override {
        program = p;
        preview = v;
    }
};

// The Program picture after `ticks` offline ticks of a loop showing only `input` on Main.
vt::Frame referencePicture(const vt::media::InputConfig& input, int64_t ticks) {
    vt::media::ProgramConfig cfg;
    cfg.inputs[0] = input;
    vt::media::ProgramLoop loop(std::move(cfg));
    LastPicture last;
    loop.addMonitor(&last);
    loop.runOffline(ticks);
    loop.removeMonitor(&last);
    return last.program;
}

// The video frames (packets) of a recording; -1 if it does not open.
int64_t videoFrames(const QString& file) {
    AVFormatContext* raw = nullptr;
    if (avformat_open_input(&raw, file.toUtf8().constData(), nullptr, nullptr) != 0) return -1;
    vt::media::FormatCtxPtr fmt(raw);
    if (avformat_find_stream_info(fmt.get(), nullptr) < 0) return -1;
    const int vi = av_find_best_stream(fmt.get(), AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (vi < 0) return -1;
    vt::media::PacketPtr pkt(av_packet_alloc());
    int64_t n = 0;
    while (av_read_frame(fmt.get(), pkt.get()) >= 0) {
        if (pkt->stream_index == vi) ++n;
        av_packet_unref(pkt.get());
    }
    return n;
}

template <class W>
W* visibleTopLevel() {
    for (QWidget* w : QApplication::topLevelWidgets())
        if (auto* t = qobject_cast<W*>(w); t && t->isVisible()) return t;
    return nullptr;
}

}  // namespace

class TestAppE2e : public QObject {
    Q_OBJECT
private slots:
    void croutonThenAutoRunsTheEffect();
    void refusalShowsInTheStatusBar();
    void effectCacheIsBounded();
    void freezeButtonFreezesTheStore();
    void grabPutsTheInputInDv1OnMain();
    void framestoreCroutonLoadsIntoDv2();
    void controlCroutonsSetPreview();
    void cgKeyPageDissolvesOverMain();
    void cgKeyPageDoubleClickAndRefusals();
    void cgCrawlPlaysOverMainAndTheRightButtonStopsIt();
    void positionableShrinkRollsOnTheTrackball();
    void saveWritesTheStillAndListsIt();
    void freezeDuringATransitionIsRefused();
    void keyedDv2IsRefusedAndAKeyedInputShows();
    void invalidDataRootThenChosenInSettings();
    void recordingIsFinishedWhenTheWindowCloses();
    void stoppingARecordingDoesNotBlock();
    void settingsChangeTheInputs();
    void logLinesFromOtherThreads();
    void audioOutputComesBack();
    void switcherKeysFromTheOtherWindows();
    void monitorGeometryIsKept();
    void menus();
    void zoomMenu();
    void zoomFollowsADevicePixelRatioChange();
    void realtimeRecordingWithLiveInputs();  // live: tools/live.bat (VT_APP_LIVE=1)
};

// Crouton "ART CARD KEY" (4000FX/Control) is its own effect: clicking it loads without an error.
// The ALGO move "Crop To Center" (4000FX/Wipes) loads. Clicking "Wipe
// Diag TL-BR" from the same drawer and then AUTO runs that wipe from In1 to In2.
void TestAppE2e::croutonThenAutoRunsTheEffect() {
    QTemporaryDir dir;
    AppSettings settings = testSettings(dir);
    AppController c(settings, Mode::Offline);
    QVERIFY(c.screen());
    const int card = showCrouton(c, "4000FX/Control", "ART CARD KEY");
    QVERIFY(card >= 0);
    QTest::mouseClick(c.screen(), Qt::LeftButton, {}, cellCentre(*c.screen(), card));
    const QString cardMessage = c.window().statusBar()->currentMessage();
    QVERIFY2(!cardMessage.contains(QStringLiteral("Cannot load")), qPrintable(cardMessage));
    c.window().statusBar()->clearMessage();
    const int crop = showCrouton(c, "4000FX/Wipes", "Crop To Center");
    QVERIFY(crop >= 0);
    QTest::mouseClick(c.screen(), Qt::LeftButton, {}, cellCentre(*c.screen(), crop));
    const QString cropMessage = c.window().statusBar()->currentMessage();
    QVERIFY2(!cropMessage.contains(QStringLiteral("Cannot load")), qPrintable(cropMessage));

    const int i = showCrouton(c, "4000FX/Wipes", "Wipe Diag TL-BR");
    QVERIFY(i >= 0);
    const fs::path file = c.screen()->project().croutons()[size_t(i)].effect;
    const int ticks = autoTicks(file);
    QVERIFY(ticks > 1);

    QTest::mouseClick(c.screen(), Qt::LeftButton, {}, cellCentre(*c.screen(), i));
    QTest::mouseClick(c.screen(), Qt::LeftButton, {}, centre(node("auto").hitRect));
    c.runOffline(ticks - 1);
    vt::media::SwitcherSnapshot s = c.loop().switcherState();
    QCOMPARE(s.effectName, vt::loadEffect(file)->name());
    QVERIFY(s.autoRunning);
    QCOMPARE(s.main, vt::Source::In1);
    QCOMPARE(c.screen()->project().loaded(), i);  // the loaded crouton is marked from the engine's effect

    c.runOffline(1);  // the tick that finishes Auto swaps Main and Preview
    s = c.loop().switcherState();
    QVERIFY(!s.autoRunning);
    QCOMPARE(s.main, vt::Source::In2);
    QCOMPARE(s.preview, vt::Source::In1);

    // The last transition frame is rendered before the swap; one more tick shows In2 plainly.
    c.runOffline(1);
    const vt::Frame want = referencePicture(settings.inputs[1], ticks + 1);
    QVERIFY(want.at(vt::kRasterW / 2, vt::kRasterH / 2).r > 0);  // a real picture, not black
    MonitorWindow& program = c.programMonitor();
    program.poll();
    QImage shown(vt::kVisibleArea.w, vt::kVisibleArea.h, QImage::Format_RGB32);
    program.render(&shown);
    for (int y = 0; y < shown.height(); ++y)
        for (int x = 0; x < shown.width(); ++x) {
            const vt::Rgba8& p = want.at(vt::kVisibleArea.x + x, vt::kVisibleArea.y + y);
            if ((shown.pixel(x, y) & 0xffffff) != (qRgb(p.r, p.g, p.b) & 0xffffff))
                QFAIL(qPrintable(QStringLiteral("Program monitor differs from In2 at (%1,%2)").arg(x).arg(y)));
        }
}

// A DVE chosen while a ChromaFX is engaged is refused: the reason shows in the status bar (again for
// a repeat), and the panel shows the engine's state, not the click.
void TestAppE2e::refusalShowsInTheStatusBar() {
    QTemporaryDir dir;
    AppSettings settings = testSettings(dir);
    AppController c(settings, Mode::Offline);
    QVERIFY(c.screen());
    const int chroma = showCrouton(c, "ChromaFx", "Fade To Neg");
    QVERIFY(chroma >= 0);
    const fs::path chromaFile = c.screen()->project().croutons()[size_t(chroma)].effect;
    QTest::mouseClick(c.screen(), Qt::LeftButton, {}, cellCentre(*c.screen(), chroma));
    QTest::mouseClick(c.screen(), Qt::LeftButton, {}, centre(node("auto").hitRect));
    c.runOffline(autoTicks(chromaFile));
    QVERIFY(c.loop().switcherState().chroma);

    const int dve = showCrouton(c, "2000FX/Blinds", "Split Corners In");
    QVERIFY(dve >= 0);
    QStatusBar* bar = c.window().statusBar();
    bar->clearMessage();
    QTest::mouseClick(c.screen(), Qt::LeftButton, {}, cellCentre(*c.screen(), dve));
    c.runOffline(1);
    QVERIFY2(bar->currentMessage().contains(QStringLiteral("ChromaFX")), qPrintable(bar->currentMessage()));

    const vt::media::SwitcherSnapshot s = c.loop().switcherState();
    QVERIFY(s.chroma);
    QCOMPARE(s.effectName, vt::loadEffect(chromaFile)->name());
    QCOMPARE(c.screen()->project().loaded(), -1);  // the DVE is not marked loaded (the ChromaFX is in another drawer)
    apptest::FakeActions fake;
    ToasterScreen engine(apptest::layout(), apptest::font(), fake);
    engine.setState(s);
    QCOMPARE(c.screen()->renderIndexedForTest(), engine.renderIndexedForTest());

    bar->clearMessage();  // the same refusal again shows again
    QTest::mouseClick(c.screen(), Qt::LeftButton, {}, cellCentre(*c.screen(), dve));
    c.runOffline(1);
    QVERIFY2(bar->currentMessage().contains(QStringLiteral("ChromaFX")), qPrintable(bar->currentMessage()));
}

// FREEZE freezes the store and lights FREEZE: the panel shows the engine's state, as published.
void TestAppE2e::freezeButtonFreezesTheStore() {
    QTemporaryDir dir;
    AppSettings settings = testSettings(dir);
    AppController c(settings, Mode::Offline);
    QVERIFY(c.screen());
    QVERIFY(!c.loop().switcherState().frozen);
    QTest::mouseClick(c.screen(), Qt::LeftButton, {}, centre(node("fs_freeze").hitRect));
    c.runOffline(1);
    const vt::media::SwitcherSnapshot s = c.loop().switcherState();
    QVERIFY(s.frozen);
    apptest::FakeActions fake;
    ToasterScreen engine(apptest::layout(), apptest::font(), fake);
    engine.setState(s);
    QCOMPARE(c.screen()->renderIndexedForTest(), engine.renderIndexedForTest());
    vt::media::SwitcherSnapshot live;
    engine.setState(live);
    QVERIFY(c.screen()->renderIndexedForTest() != engine.renderIndexedForTest());  // FREEZE is lit, LIVE is not

    QTest::mouseClick(c.screen(), Qt::LeftButton, {}, centre(node("fs_live").hitRect));
    c.runOffline(1);
    QVERIFY(!c.loop().switcherState().frozen);
}

// Grab 1 stills In1 into DV1 and selects DV1 on Main once the grab's wait is over.
void TestAppE2e::grabPutsTheInputInDv1OnMain() {
    QTemporaryDir dir;
    AppSettings settings = testSettings(dir);
    AppController c(settings, Mode::Offline);
    QVERIFY(c.screen());
    QTest::mouseClick(c.screen(), Qt::LeftButton, {}, centre(node("fs_1").hitRect));
    c.runOffline(6);
    const vt::media::SwitcherSnapshot s = c.loop().switcherState();
    QCOMPARE(s.main, vt::Source::DV1);
    QVERIFY(s.frozen);
}

// A FRAM crouton loads into a DV (DV2 here: none is shown yet) and is shown frozen on Preview, at
// the framestore's place in the raster (x0 = 8, y0 = 1): the ColorBars white and yellow bars.
void TestAppE2e::framestoreCroutonLoadsIntoDv2() {
    QTemporaryDir dir;
    AppSettings settings = testSettings(dir);
    AppController c(settings, Mode::Offline);
    QVERIFY(c.screen());
    LastPicture last;
    c.loop().addMonitor(&last);
    const int bars = showCrouton(c, "Framestore", "ColorBars");
    QVERIFY(bars >= 0);
    QTest::mouseClick(c.screen(), Qt::LeftButton, {}, cellCentre(*c.screen(), bars));
    c.runOffline(1);
    c.loop().removeMonitor(&last);
    const vt::media::SwitcherSnapshot s = c.loop().switcherState();
    QCOMPARE(s.preview, vt::Source::DV2);
    QVERIFY(s.frozen);
    auto near = [](vt::Rgba8 p, int r, int g, int b) {
        return std::abs(p.r - r) <= 2 && std::abs(p.g - g) <= 2 && std::abs(p.b - b) <= 2;
    };
    const vt::Rgba8 white = last.preview.at(61, 121), yellow = last.preview.at(61 + 107, 121);
    QVERIFY2(near(white, 191, 191, 191), qPrintable(QStringLiteral("%1 %2 %3").arg(white.r).arg(white.g).arg(white.b)));
    QVERIFY2(near(yellow, 191, 191, 0), qPrintable(QStringLiteral("%1 %2 %3").arg(yellow.r).arg(yellow.g).arg(yellow.b)));
}

// Control croutons (FREEZE.md 4): Video Source 3 puts In3 on Preview; Video Source Cyan sets the
// BKG colour and puts BKG on Preview; Stop only says it is a sequencer control. The effect stays.
void TestAppE2e::controlCroutonsSetPreview() {
    QTemporaryDir dir;
    AppSettings settings = testSettings(dir);
    AppController c(settings, Mode::Offline);
    QVERIFY(c.screen());
    auto click = [&](const char* name) {
        const int i = showCrouton(c, "4000FX/Control", name);
        QVERIFY(i >= 0);
        QTest::mouseClick(c.screen(), Qt::LeftButton, {}, cellCentre(*c.screen(), i));
        c.runOffline(1);
    };
    click("Video Source 3");
    QCOMPARE(c.loop().switcherState().preview, vt::Source::In3);
    click("Video Source Cyan");
    vt::media::SwitcherSnapshot s = c.loop().switcherState();
    QCOMPARE(s.preview, vt::Source::Bkg);
    QVERIFY(s.background == vt::EffectColour::of(vt::BkgColour::Cyan));
    QCOMPARE(s.main, vt::Source::In1);
    click("Stop");
    QCOMPARE(c.loop().switcherState().preview, vt::Source::Bkg);
    QVERIFY(c.window().statusBar()->currentMessage().contains(QStringLiteral("project control")));
}

// docs/2026-10-30-cg-keys-design.md: CG/Keys/LowerThird1 loads into DV2 with nothing on air and is
// marked loaded; AUTO dissolves it on: Program shows its text area over Main, every pixel outside its
// alpha still Main; the next AUTO dissolves it off and the plain Fade is left (no crouton marked).
void TestAppE2e::cgKeyPageDissolvesOverMain() {
    QTemporaryDir dir;
    AppSettings settings = testSettings(dir);
    AppController c(settings, Mode::Offline);
    QVERIFY(c.screen());
    LastPicture last;
    c.loop().addMonitor(&last);
    const int i = showCrouton(c, "CG/Keys", "LowerThird1");
    QVERIFY(i >= 0);
    QTest::mouseClick(c.screen(), Qt::LeftButton, {}, cellCentre(*c.screen(), i));
    c.runOffline(1);
    vt::media::SwitcherSnapshot s = c.loop().switcherState();
    QCOMPARE(s.cgPage, (vt::CgPageState{vt::Source::DV2, false, 0, true}));
    QCOMPARE(s.superimpose, vt::Source::None);
    QCOMPARE(c.screen()->project().loaded(), i);

    QTest::mouseClick(c.screen(), Qt::LeftButton, {}, centre(node("auto").hitRect));
    c.runOffline(64);  // 60 >> 2 = 15 steps + 1, 4 fields each
    s = c.loop().switcherState();
    QVERIFY(!s.autoRunning);
    QCOMPARE(s.cgPage, (vt::CgPageState{vt::Source::DV2, true, 255, true}));
    QCOMPARE(s.keyMode, vt::KeyMode::Off);

    const vt::Frame main = referencePicture(settings.inputs[0], 65);
    const auto page = vt::loadCgKeyPage(c.screen()->project().croutons()[size_t(i)].effect);
    QVERIFY(page);
    int changed = 0;
    for (int y = 0; y < vt::kRasterH; ++y)
        for (int x = 0; x < vt::kRasterW; ++x) {
            const bool same = last.program.at(x, y) == main.at(x, y);
            if (page->alpha[size_t(y) * vt::kRasterW + size_t(x)] == 0) {
                if (!same) QFAIL(qPrintable(QStringLiteral("Program is not Main at (%1,%2)").arg(x).arg(y)));
            } else {
                QVERIFY2(y >= 44 + 337 && y <= 44 + 398, qPrintable(QStringLiteral("alpha at row %1").arg(y)));
                changed += !same;
            }
        }
    QVERIFY2(changed > 20000, qPrintable(QString::number(changed)));  // the bar and the text

    QTest::mouseClick(c.screen(), Qt::LeftButton, {}, centre(node("auto").hitRect));
    c.runOffline(64);
    s = c.loop().switcherState();
    QCOMPARE(s.cgPage, vt::CgPageState{});
    QCOMPARE(s.superimpose, vt::Source::None);
    QCOMPARE(s.effectName, vt::makeDissolve()->name());
    QCOMPARE(c.screen()->project().loaded(), -1);
    c.loop().removeMonitor(&last);
}

// A double click loads the page and cuts it on; f and l are refused while it is loaded (the status
// bar says so) while the LIVE button works; a control crouton clears it.
void TestAppE2e::cgKeyPageDoubleClickAndRefusals() {
    QTemporaryDir dir;
    AppSettings settings = testSettings(dir);
    AppController c(settings, Mode::Offline);
    QVERIFY(c.screen());
    const int i = showCrouton(c, "CG/Keys", "KeyPage2");
    QVERIFY(i >= 0);
    QTest::mouseDClick(c.screen(), Qt::LeftButton, {}, cellCentre(*c.screen(), i));
    c.runOffline(1);
    vt::media::SwitcherSnapshot s = c.loop().switcherState();
    QCOMPARE(s.cgPage, (vt::CgPageState{vt::Source::DV2, true, 255, true}));
    QCOMPARE(s.preview, vt::Source::In2);

    QStatusBar* bar = c.window().statusBar();
    bar->clearMessage();
    QTest::keyClick(c.screen(), Qt::Key_L);
    c.runOffline(1);
    c.poll();
    QVERIFY2(bar->currentMessage().contains(QStringLiteral("CG key page")), qPrintable(bar->currentMessage()));
    QVERIFY(c.loop().switcherState().frozen);
    QTest::mouseClick(c.screen(), Qt::LeftButton, {}, centre(node("fs_live").hitRect));
    c.runOffline(1);
    QVERIFY(!c.loop().switcherState().frozen);

    const int ctl = showCrouton(c, "4000FX/Control", "Video Source 3");
    QVERIFY(ctl >= 0);
    QTest::mouseClick(c.screen(), Qt::LeftButton, {}, cellCentre(*c.screen(), ctl));
    c.runOffline(1);
    s = c.loop().switcherState();
    QCOMPARE(s.cgPage, vt::CgPageState{});
    QCOMPARE(s.superimpose, vt::Source::None);
    QCOMPARE(s.preview, vt::Source::In3);
}

// docs/2026-10-30-cg-scrolls-design.md: a double click on CG/Crawls/Novel.Crawl plays it at once. After
// some fields its band (frame lines 104..403) shows text over Main while every row outside it is still
// Main; Take is ignored with the status note while it plays; the right button stops it, the page stays
// loaded.
void TestAppE2e::cgCrawlPlaysOverMainAndTheRightButtonStopsIt() {
    QTemporaryDir dir;
    AppSettings settings = testSettings(dir);
    AppController c(settings, Mode::Offline);
    QVERIFY(c.screen());
    LastPicture last;
    c.loop().addMonitor(&last);
    const int i = showCrouton(c, "CG/Crawls", "Novel.Crawl");
    QVERIFY(i >= 0);
    QStatusBar* bar = c.window().statusBar();
    QTest::mouseDClick(c.screen(), Qt::LeftButton, {}, cellCentre(*c.screen(), i));
    QTest::keyClick(c.screen(), Qt::Key_Return);  // before any snapshot shows the play: ignored all the same
    QVERIFY2(bar->currentMessage().contains(QStringLiteral("right-click stops it")), qPrintable(bar->currentMessage()));
    c.runOffline(1);
    vt::media::SwitcherSnapshot s = c.loop().switcherState();
    QCOMPARE(s.cgScroll, (vt::CgScrollState{true, true}));
    QCOMPARE(s.main, vt::Source::In1);
    QCOMPARE(s.effectName, vt::makeDissolve()->name());
    QVERIFY(s.frozen);
    QCOMPARE(c.screen()->project().loaded(), i);

    c.runOffline(59);  // fields 0..59: 16 px per field
    const vt::Frame main = referencePicture(settings.inputs[0], 60);
    int changed = 0;
    for (int y = 0; y < vt::kRasterH; ++y)
        for (int x = 0; x < vt::kRasterW; ++x) {
            const bool same = last.program.at(x, y) == main.at(x, y);
            if (y < 104 || y >= 404) {
                if (!same) QFAIL(qPrintable(QStringLiteral("Program is not Main at (%1,%2)").arg(x).arg(y)));
            } else {
                changed += !same;
            }
        }
    QVERIFY2(changed > 5000, qPrintable(QString::number(changed)));

    bar->clearMessage();
    QTest::keyClick(c.screen(), Qt::Key_Return);  // Take: ignored
    c.runOffline(1);
    s = c.loop().switcherState();
    QVERIFY2(bar->currentMessage().contains(QStringLiteral("right-click stops it")), qPrintable(bar->currentMessage()));
    QCOMPARE(s.main, vt::Source::In1);
    QVERIFY(s.cgScroll.playing);

    QTest::mouseClick(c.screen(), Qt::RightButton, {}, centre(node("main_3").hitRect));
    c.runOffline(1);
    s = c.loop().switcherState();
    QCOMPARE(s.cgScroll, (vt::CgScrollState{true, false}));
    QCOMPARE(s.tbar, 0);
    QCOMPARE(last.program.px, referencePicture(settings.inputs[0], 62).px);
    QCOMPARE(c.screen()->project().loaded(), i);
    c.loop().removeMonitor(&last);
}

// tools/re/POSITION.md: Positionable Shrink with the T-bar let go mid-way is positioned with the
// Position dock's trackball; Finish keeps the position and ends the take.
void TestAppE2e::positionableShrinkRollsOnTheTrackball() {
    QTemporaryDir dir;
    AppSettings settings = testSettings(dir);
    AppController c(settings, Mode::Offline);
    QVERIFY(c.screen());
    ToasterScreen& w = *c.screen();
    PositionDock& dock = c.positionDock();
    QCOMPARE(dock.windowTitle(), QStringLiteral("Position"));
    QVERIFY(!dock.isHidden());
    QVERIFY(c.window().windowMenu()->actions().contains(dock.toggleViewAction()));
    const int i = showCrouton(c, "4000FX/Special", "Positionable Shrink");
    QVERIFY(i >= 0);
    QTest::mouseClick(&w, Qt::LeftButton, {}, cellCentre(w, i));
    c.runOffline(1);
    QVERIFY(!dock.ball()->isEnabled());
    auto position = [&] {
        vt::EffectPosition p;
        c.loop().post([&p](vt::Switcher& sw) { p = sw.position(); });
        c.runOffline(1);
        return p;
    };
    const vt::Rect r = apptest::layout().tbarRect;
    const QPoint mid(r.x + r.w / 2, r.y + r.h / 2);
    QTest::mousePress(&w, Qt::LeftButton, {}, mid);
    QTest::mouseRelease(&w, Qt::LeftButton, {}, mid);
    c.runOffline(1);
    QVERIFY(c.loop().switcherState().positioning);
    QVERIFY(dock.ball()->isEnabled());
    QCOMPARE(QWidget::mouseGrabber(), nullptr);  // the pointer stays free

    const QPoint at = dock.ball()->rect().center();
    QTest::mousePress(dock.ball(), Qt::LeftButton, {}, at);
    QTest::mouseMove(dock.ball(), at + QPoint(10, 5));
    QTest::mouseRelease(dock.ball(), Qt::LeftButton, {}, at + QPoint(10, 5));
    const vt::EffectPosition p = position();
    QCOMPARE(p.h, 5);  // kind 3: half a control cell per count, rounded away from 0
    QCOMPARE(p.v, 3);

    dock.finishButton()->click();
    c.runOffline(1);
    const vt::media::SwitcherSnapshot s = c.loop().switcherState();
    QVERIFY(!s.positioning);
    QVERIFY(!s.transition);
    QCOMPARE(s.main, vt::Source::In2);
    QVERIFY(!dock.ball()->isEnabled());
    QCOMPARE(position().h, 5);  // kept with the effect
}

// SAVE writes <path> and <path>.I from the still in DV1/DV2 and lists it in "Saved framestores".
// Without a still, or during an effect, it writes nothing and the status bar says why.
void TestAppE2e::saveWritesTheStillAndListsIt() {
    QTemporaryDir dir;
    AppSettings settings = testSettings(dir);
    AppController c(settings, Mode::Offline);
    QVERIFY(c.screen());
    QStatusBar* bar = c.window().statusBar();
    const QString test1 = dir.filePath(QStringLiteral("Saved/Test1"));
    // SAVE's button refuses before its dialog: a dialog that opens anyway is closed and noted
    auto saveRefusedWithoutDialog = [&](const QString& why) {
        auto dialog = std::make_shared<bool>(false);
        QTimer::singleShot(500, &c, [dialog] {
            if (QWidget* w = QApplication::activeModalWidget()) {
                *dialog = true;
                w->close();
            }
        });
        bar->clearMessage();
        c.saveFramestore();
        QVERIFY(!*dialog);
        QVERIFY2(bar->currentMessage().contains(why), qPrintable(bar->currentMessage()));
    };
    c.runOffline(1);
    saveRefusedWithoutDialog(QStringLiteral("SAVE: no still in DV1/DV2"));

    QVERIFY(c.saveFramestore(test1));
    c.runOffline(1);
    QCoreApplication::processEvents();
    QVERIFY2(bar->currentMessage().contains(QStringLiteral("SAVE: no still in DV1/DV2")), qPrintable(bar->currentMessage()));
    QVERIFY(!QFileInfo::exists(test1));

    const int bars = showCrouton(c, "Framestore", "ColorBars");
    QVERIFY(bars >= 0);
    QTest::mouseClick(c.screen(), Qt::LeftButton, {}, cellCentre(*c.screen(), bars));
    c.runOffline(1);
    c.setTBar(100);
    c.runOffline(1);
    saveRefusedWithoutDialog(QStringLiteral("finish the effect first"));
    bar->clearMessage();
    QVERIFY(c.saveFramestore(test1));
    c.runOffline(1);
    QCoreApplication::processEvents();
    QVERIFY2(bar->currentMessage().contains(QStringLiteral("finish the effect first")), qPrintable(bar->currentMessage()));
    QVERIFY(!QFileInfo::exists(test1));

    c.setTBar(0);
    c.runOffline(1);
    QVERIFY(c.saveFramestore(test1));
    c.runOffline(1);
    QCoreApplication::processEvents();
    QVERIFY(QFileInfo(test1).isFile());
    QVERIFY(QFileInfo(test1 + QStringLiteral(".I")).isFile());
    QCOMPARE(bar->currentMessage(), QStringLiteral("Saved Test1"));

    QComboBox* picker = c.projectView()->drawerPicker();
    picker->setCurrentIndex(picker->findText(QStringLiteral("Saved framestores")));
    const auto& croutons = c.screen()->project().croutons();
    QCOMPARE(croutons.size(), size_t(1));
    QCOMPARE(croutons[0].name, std::string("Test1"));
    // It is a framestore crouton like the shipped ones: loading it again shows no error.
    bar->clearMessage();
    c.loadCrouton(0, false);
    c.runOffline(1);
    QVERIFY2(bar->currentMessage().isEmpty(), qPrintable(bar->currentMessage()));
}

// FREEZE with the T-bar at 100 shows the engine's refusal.
void TestAppE2e::freezeDuringATransitionIsRefused() {
    QTemporaryDir dir;
    AppSettings settings = testSettings(dir);
    AppController c(settings, Mode::Offline);
    QVERIFY(c.screen());
    c.setTBar(100);
    c.runOffline(1);
    c.window().statusBar()->clearMessage();
    c.setFrozen(true);
    c.runOffline(1);
    QVERIFY2(c.window().statusBar()->currentMessage().contains(QStringLiteral("finish the effect first")),
             qPrintable(c.window().statusBar()->currentMessage()));
}

// DV2 alone cannot be keyed while frozen: the refusal shows in the status bar. In3 on Superimpose
// puts the key on (White).
void TestAppE2e::keyedDv2IsRefusedAndAKeyedInputShows() {
    QTemporaryDir dir;
    AppSettings settings = testSettings(dir);
    AppController c(settings, Mode::Offline);
    QVERIFY(c.screen());
    QTest::mouseClick(c.screen(), Qt::LeftButton, {}, centre(node("fs_freeze").hitRect));
    c.runOffline(1);
    QTest::mouseClick(c.screen(), Qt::LeftButton, {}, centre(node("sup_dv2").hitRect));
    c.runOffline(1);
    QCOMPARE(c.window().statusBar()->currentMessage(), QStringLiteral("Refused: Superimpose: DV2 alone cannot be keyed"));
    QTest::mouseClick(c.screen(), Qt::LeftButton, {}, centre(node("sup_3").hitRect));
    c.runOffline(1);
    const vt::media::SwitcherSnapshot s = c.loop().switcherState();
    QCOMPARE(s.superimpose, vt::Source::In3);
    QCOMPARE(s.keyMode, vt::KeyMode::White);
}

// The controller keeps the kEffectCache (12) most recently clicked croutons loaded. A DVE move
// running from Auto drops out of that cache while 19 more DVE croutons are clicked (each refused: a
// DVE move is running), and the engine still plays it to the end.
void TestAppE2e::effectCacheIsBounded() {
    QTemporaryDir dir;
    AppSettings settings = testSettings(dir);
    AppController c(settings, Mode::Offline);
    QVERIFY(c.screen());
    // 20 DVE croutons (drawer, index), the drawers in picker order.
    QComboBox* picker = c.projectView()->drawerPicker();
    std::vector<std::pair<int, int>> dves;
    std::shared_ptr<const vt::Effect> first;
    for (int d = 0; d < picker->count() && dves.size() < 20; ++d) {
        picker->setCurrentIndex(d);
        const auto& croutons = c.screen()->project().croutons();
        for (size_t i = 0; i < croutons.size() && dves.size() < 20; ++i)
            if (auto e = vt::loadEffect(croutons[i].effect); e && e->usesDvMemory()) {
                dves.emplace_back(d, int(i));
                if (!first) first = std::move(e);
            }
    }
    QCOMPARE(dves.size(), size_t(20));
    auto load = [&](const std::pair<int, int>& at) {
        picker->setCurrentIndex(at.first);
        c.loadCrouton(at.second, false);
    };
    const vt::Timing timing = first->timing();
    const int ticks = timing.durationTicks[size_t(timing.resolve(vt::Speed::Slow))];
    QVERIFY(ticks > 3);

    load(dves[0]);
    c.setSpeed(vt::Speed::Slow);
    c.autoTransition(vt::Direction::Forward);
    c.runOffline(1);
    vt::media::SwitcherSnapshot s = c.loop().switcherState();
    QVERIFY(s.autoRunning);
    QCOMPARE(s.effectName, first->name());
    const uint64_t refusals = s.refusalCount;
    for (size_t k = 1; k < dves.size(); ++k) {
        load(dves[k]);
        QVERIFY(c.cachedEffects() <= AppController::kEffectCache);
    }
    QCOMPARE(c.cachedEffects(), AppController::kEffectCache);

    c.runOffline(1);
    s = c.loop().switcherState();
    QCOMPARE(s.refusalCount, refusals + 19);
    QCOMPARE(s.effectName, first->name());
    QVERIFY(s.autoRunning);
    c.runOffline(ticks - 3);
    QVERIFY(c.loop().switcherState().autoRunning);
    c.runOffline(1);
    s = c.loop().switcherState();
    QVERIFY(!s.autoRunning);
    QCOMPARE(s.main, vt::Source::In2);
    QCOMPARE(s.effectName, first->name());
}

// Review Focus 2: a stored data root that does not exist shows the message and a button that opens
// Settings on the Toaster data page; choosing a valid root there brings the panel up at once.
void TestAppE2e::invalidDataRootThenChosenInSettings() {
    QTemporaryDir dir;
    AppSettings settings = testSettings(dir);
    settings.toasterRoot = dir.filePath(QStringLiteral("moved-away"));
    AppController c(settings, Mode::Offline);
    QVERIFY(!c.screen());
    QPointer<QWidget> msg = c.window().findChild<QWidget*>(QStringLiteral("dataRootMessage"));
    QVERIFY(msg);
    auto* label = msg->findChild<QLabel*>();
    QVERIFY(label);
    QVERIFY2(label->text().startsWith(QStringLiteral("Toaster data not found at ") + QDir::toNativeSeparators(settings.toasterRoot)),
             qPrintable(label->text()));

    for (int i = 0; i < 3; ++i)  // the main window, the dialog, and the window again once the panel is in
        QTest::ignoreMessage(QtWarningMsg, "This plugin does not support propagateSizeHints()");
    c.window().show();
    QVERIFY(QTest::qWaitForWindowExposed(&c.window()));
    QTest::mouseClick(msg->findChild<QPushButton*>(), Qt::LeftButton);
    SettingsDialog* dialog = nullptr;
    QTRY_VERIFY((dialog = visibleTopLevel<SettingsDialog>()) != nullptr);
    auto* root = dialog->findChild<QLineEdit*>(QStringLiteral("toasterRoot"));
    QVERIFY(root);
    QVERIFY(root->isVisible());  // the Toaster data page is the one shown
    root->setText(dataRoot());
    QTest::mouseClick(dialog->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok), Qt::LeftButton);

    QVERIFY(c.screen());
    QVERIFY(!msg);
    QTRY_VERIFY(c.screen()->isVisible());
    AppSettings stored(settings.file());
    stored.load();
    QCOMPARE(stored.toasterRoot, dataRoot());
}

// Review Focus 4: closing the main window while recording finishes the file, with every tick in it,
// and returns within 3 s.
void TestAppE2e::recordingIsFinishedWhenTheWindowCloses() {
    QTemporaryDir dir;
    AppSettings settings = testSettings(dir);
    auto c = std::make_unique<AppController>(settings, Mode::Offline);
    QSignalSpy finished(c.get(), &AppController::finished);
    c->startRecording();
    QVERIFY(c->recording());
    QCOMPARE(c->window().recordAction()->text(), QStringLiteral("Stop Recording"));
    c->runOffline(120);

    QElapsedTimer t;
    t.start();
    c->window().close();
    QCOMPARE(finished.count(), 1);
    c.reset();
    QVERIFY2(t.elapsed() < 3000, qPrintable(QString::number(t.elapsed())));

    const QString file = QFileInfo(dir.filePath(QStringLiteral("rec"))).absoluteFilePath();
    const QStringList found = QDir(file).entryList({QStringLiteral("vtoast-*")}, QDir::Files);
    QCOMPARE(found.size(), 1);
    QVERIFY2(QRegularExpression(QStringLiteral("^vtoast-\\d{8}-\\d{6}\\.mkv$")).match(found[0]).hasMatch(), qPrintable(found[0]));
    QCOMPARE(videoFrames(QDir(file).filePath(found[0])), int64_t(120));
}

// Stop runs Recorder::stop() on another thread: the menu reads "Stopping…" until the file is done.
void TestAppE2e::stoppingARecordingDoesNotBlock() {
    QTemporaryDir dir;
    AppSettings settings = testSettings(dir);
    settings.recordingFormat = vt::media::RecordFormat::Ffv1Mkv;
    AppController c(settings, Mode::Offline);
    QAction* record = c.window().recordAction();
    record->trigger();
    QVERIFY(c.recording());
    c.runOffline(30);
    record->trigger();
    QCOMPARE(record->text(), QStringLiteral("Stopping…"));
    QVERIFY(!record->isEnabled());
    QTRY_COMPARE(record->text(), QStringLiteral("Start Recording"));
    QVERIFY(record->isEnabled());
    QVERIFY(!c.recording());
    QCOMPARE(videoFrames(c.recordingFile()), int64_t(30));
}

void TestAppE2e::settingsChangeTheInputs() {
    QTemporaryDir dir;
    AppSettings settings = testSettings(dir);
    settings.inputs[1] = {};
    AppController c(settings, Mode::Offline);
    c.runOffline(2);
    QCOMPARE(c.loop().inputStatus(1).state, vt::media::InputStatus::State::Idle);
    AppSettings next = settings;
    next.inputs[1] = fileInput("f25_169.mkv");
    c.applySettings(next);
    c.runOffline(2);
    QCOMPARE(c.loop().inputStatus(1).state, vt::media::InputStatus::State::Running);
    QVERIFY(c.window().findChild<QLabel*>(QStringLiteral("inputStatus"))->text().contains(QStringLiteral("In2 on")));
}

// The log sink only queues (it runs under a global lock on worker threads); poll() shows the lines.
void TestAppE2e::logLinesFromOtherThreads() {
    QTemporaryDir dir;
    AppSettings settings = testSettings(dir);
    AppController c(settings, Mode::Offline);
    std::thread([] { vt::logWarning("a warning from a worker thread"); }).join();
    QVERIFY(!c.logView()->toPlainText().contains(QStringLiteral("worker thread")));
    c.poll();
    QVERIFY(c.logView()->toPlainText().contains(QStringLiteral("a warning from a worker thread")));
    QVERIFY(c.window().statusBar()->currentMessage().contains(QStringLiteral("worker thread")));

    c.notImplemented(QStringLiteral("FREEZE"));
    QCOMPARE(c.window().statusBar()->currentMessage(), QStringLiteral("FREEZE: not in version 1"));
}

// The audio output that does not start, or goes away (an unplugged headset), shows "No audio output"
// in the status bar until a retry, every 2 s, starts it again; applying the settings restarts it too.
void TestAppE2e::audioOutputComesBack() {
    QTemporaryDir dir;
    AppSettings settings = testSettings(dir);
    auto fake = std::make_unique<FakeAudio>();
    FakeAudio* audio = fake.get();
    audio->failures = 1;
    AppController c(settings, Mode::Offline, nullptr, std::move(fake));
    auto* label = c.window().findChild<QLabel*>(QStringLiteral("audioStatus"));
    QVERIFY(label);
    QCOMPARE(audio->starts, 1);
    QVERIFY(!label->isHidden());
    QCOMPARE(label->text(), QStringLiteral("No audio output"));
    QCOMPARE(label->toolTip(), QStringLiteral("no playback device"));
    QCOMPARE(c.window().statusBar()->currentMessage(), QStringLiteral("No audio output: no playback device"));

    QElapsedTimer t;
    t.start();
    QTRY_VERIFY_WITH_TIMEOUT(label->isHidden(), 4000);
    QVERIFY2(t.elapsed() >= AppController::kAudioRetryMs - 100, qPrintable(QString::number(t.elapsed())));
    QCOMPARE(audio->starts, 2);
    QVERIFY(audio->running());

    audio->on = false;  // the device went away
    c.poll();
    QVERIFY(!label->isHidden());
    QCOMPARE(audio->starts, 2);  // the last try was just now
    QTRY_VERIFY_WITH_TIMEOUT(label->isHidden(), 4000);
    QCOMPARE(audio->starts, 3);

    audio->on = false;  // and once more: applying the settings starts it at once
    c.applySettings(settings);
    QCOMPARE(audio->starts, 4);
    QVERIFY(audio->running());
}

// The Switcher's keys work while a monitor or the main window is the active window, whichever of
// their widgets has the focus; a text field keeps its keys, and the mixer's mute buttons take no focus.
void TestAppE2e::switcherKeysFromTheOtherWindows() {
    QTemporaryDir dir;
    AppSettings settings = testSettings(dir);
    AppController c(settings, Mode::Offline);
    QVERIFY(c.screen());
    auto mainSource = [&] { return c.loop().switcherState().main; };
    QCOMPARE(mainSource(), vt::Source::In1);

    QTest::keyClick(&c.programMonitor(), Qt::Key_Return);  // Take
    c.runOffline(1);
    QCOMPARE(mainSource(), vt::Source::In2);
    QTest::keyClick(&c.previewMonitor(), Qt::Key_F4);  // Main = In4
    c.runOffline(1);
    QCOMPARE(mainSource(), vt::Source::In4);
    QTest::keyClick(c.mixerDock().fader(vt::media::Channel::Master), Qt::Key_F1);
    c.runOffline(1);
    QCOMPARE(mainSource(), vt::Source::In1);
    QTest::keyClick(c.projectView()->drawerPicker(), Qt::Key_F3);
    c.runOffline(1);
    QCOMPARE(mainSource(), vt::Source::In3);
    for (int i = 0; i < MixerDock::kChannels; ++i)
        QCOMPARE(c.mixerDock().muteButton(vt::media::Channel(i))->focusPolicy(), Qt::NoFocus);

    // A text field with the focus keeps the keys, also those it does not use (they go on to its parents).
    auto* edit = new QLineEdit(c.projectView());
    QTest::ignoreMessage(QtWarningMsg, "This plugin does not support propagateSizeHints()");
    c.window().show();
    QVERIFY(QTest::qWaitForWindowActive(&c.window()));
    edit->setFocus();
    QCOMPARE(QApplication::focusWidget(), edit);
    QTest::keyClick(edit, Qt::Key_F1);
    QTest::keyClick(edit, Qt::Key_Q);
    c.runOffline(1);
    QCOMPARE(mainSource(), vt::Source::In3);
    QCOMPARE(c.loop().switcherState().superimpose, vt::Source::None);
    QCOMPARE(edit->text(), QStringLiteral("q"));
}

// The monitor windows' places are stored when the app closes and restored at the next start.
void TestAppE2e::monitorGeometryIsKept() {
    QTemporaryDir dir;
    AppSettings settings = testSettings(dir);
    {
        AppController c(settings, Mode::Offline);
        c.programMonitor().move(10, 20);
        c.previewMonitor().move(100, 30);
        c.shutdown();
    }
    QVERIFY(!settings.programGeometry.isEmpty());
    AppSettings stored(settings.file());
    stored.load();
    QCOMPARE(stored.programGeometry, settings.programGeometry);
    QCOMPARE(stored.previewGeometry, settings.previewGeometry);
    AppController c(stored, Mode::Offline);
    // Where restoreGeometry() puts a window (the offscreen platform shifts an unshown one a little).
    auto restored = [](const QByteArray& g) {
        MonitorWindow w(MonitorWindow::Kind::Program);
        w.restoreGeometry(g);
        return w.pos();
    };
    QCOMPARE(c.programMonitor().pos(), restored(stored.programGeometry));
    QCOMPARE(c.previewMonitor().pos(), restored(stored.previewGeometry));
    QVERIFY((c.previewMonitor().pos() - QPoint(100, 30)).manhattanLength() < 10);
    QCOMPARE(c.previewMonitor().size(), c.previewMonitor().sizeHint());
}

void TestAppE2e::menus() {
    QTemporaryDir dir;
    AppSettings settings = testSettings(dir);
    AppController c(settings, Mode::Offline);
    MainWindow& w = c.window();
    QCOMPARE(w.settingsAction()->text(), QStringLiteral("&Settings…"));
    QCOMPARE(w.recordAction()->text(), QStringLiteral("Start Recording"));
    QCOMPARE(w.recordAction()->shortcut(), QKeySequence(Qt::CTRL | Qt::Key_R));
    QCOMPARE(w.quitAction()->text(), QStringLiteral("&Quit"));
    QCOMPARE(w.logAction()->text(), QStringLiteral("&Log"));
}

// View -> Zoom: Default, then 1x up to the fit for the display or the stored zoom (the offscreen display
// fits only 1x; the stored zoom is 3), the stored choice checked. The window opens with the panel at
// the stored zoom; a choice sets the setting and resizes the window so the panel is at that zoom.
// Resized by hand, the window keeps its size and the panel snaps to the zoom that fits, never below 1x.
void TestAppE2e::zoomMenu() {
    QTemporaryDir dir;
    AppSettings settings = testSettings(dir);
    settings.screenZoom = 3;
    AppController c(settings, Mode::Offline);
    QVERIFY(c.screen());
    QCOMPARE(c.screen()->sizeHint(), QSize(768 * 3, 481 * 3));  // offscreen: devicePixelRatio 1
    QTest::ignoreMessage(QtWarningMsg, "This plugin does not support propagateSizeHints()");
    QTest::ignoreMessage(QtWarningMsg, "This plugin does not support raise()");
    c.show();
    QVERIFY(QTest::qWaitForWindowExposed(&c.window()));
    QTRY_COMPARE(c.screen()->zoom(), 3);
    QCOMPARE(c.screen()->size(), QSize(768 * 3, 481 * 3));
    QStringList names;
    QAction *two = nullptr, *three = nullptr, *fit = nullptr;
    for (QAction* a : c.window().zoomMenu()->actions()) {
        if (a->isSeparator()) continue;
        names << a->text();
        if (a->text() == QStringLiteral("2×")) two = a;
        if (a->text() == QStringLiteral("3×")) three = a;
        if (a->text() == QStringLiteral("Default")) fit = a;
        QCOMPARE(a->isChecked(), a->text() == QStringLiteral("3×"));
    }
    QCOMPARE(names, (QStringList{"Default", "1×", "2×", "3×"}));
    two->trigger();
    QCOMPARE(settings.screenZoom, 2);
    QVERIFY(two->isChecked());
    QTRY_COMPARE(c.screen()->zoom(), 2);
    QCOMPARE(c.screen()->size(), QSize(768 * 2, 481 * 2));
    three->trigger();
    QCOMPARE(settings.screenZoom, 3);
    QTRY_COMPARE(c.screen()->zoom(), 3);
    QCOMPARE(c.screen()->size(), QSize(768 * 3, 481 * 3));
    fit->trigger();
    QCOMPARE(settings.screenZoom, 0);
    QTRY_COMPARE(c.screen()->zoom(), 1);  // the default: all the offscreen display fits

    // By hand: larger snaps up, the size is kept; smaller stops at the 1x panel.
    const QSize one = c.screen()->size();
    c.window().resize(c.window().size() + QSize(768 + 40, 481 + 20));
    QTRY_COMPARE(c.screen()->zoom(), 2);
    QCOMPARE(c.screen()->size(), one + QSize(768 + 40, 481 + 20));
    QCOMPARE(settings.screenZoom, 0);
    c.window().resize(100, 100);
    QTRY_COMPARE(c.screen()->size(), one);  // the smallest: 768 wide, as tall as the mixer needs
    QCOMPARE(one.width(), 768);
    QVERIFY(one.height() >= 481);
    QCOMPARE(c.screen()->zoom(), 1);
}

// Moved to another display: with Default (screenZoom 0) the zoom is picked anew for the display the
// window is on, and the window takes the new size. With a chosen zoom the window keeps its size (the
// panel snaps to what fits it).
void TestAppE2e::zoomFollowsADevicePixelRatioChange() {
    QTemporaryDir dir;
    AppSettings settings = testSettings(dir);
    settings.screenZoom = 0;
    AppController c(settings, Mode::Offline);
    QVERIFY(c.screen());
    QCOMPARE(c.screen()->sizeHint(), QSize(768, 481));  // the default: all the offscreen display fits
    QTest::ignoreMessage(QtWarningMsg, "This plugin does not support propagateSizeHints()");
    c.window().show();
    QVERIFY(QTest::qWaitForWindowExposed(&c.window()));
    c.screen()->setZoom(3);  // as picked on the display the window came from
    QTRY_VERIFY(c.window().sizeHint().width() >= 2304);
    c.window().resize(c.window().sizeHint());
    QTRY_COMPARE(c.screen()->zoom(), 3);
    apptest::sendRatioChange(c.window());
    QTRY_COMPARE(c.screen()->zoom(), 1);
    QCOMPARE(settings.screenZoom, 0);

    c.setScreenZoom(2);
    QTRY_COMPARE(c.screen()->zoom(), 2);
    c.window().resize(c.window().size() + QSize(100, 50));
    const QSize kept = c.window().size();
    apptest::sendRatioChange(c.window());
    QCoreApplication::processEvents();
    QCOMPARE(c.window().size(), kept);
    QCOMPARE(c.screen()->zoom(), 2);
}

// Review Focus 4 in real time: In1 = the desktop, In2 = the first video device, the audio output
// playing; a dissolve and a crouton wipe while recording 10 s, then the main window closes. The
// file must hold about 10 s of frames and everything must be down within 3 s.
void TestAppE2e::realtimeRecordingWithLiveInputs() {
    if (qEnvironmentVariableIsEmpty("VT_APP_LIVE")) QSKIP("live: needs a desktop and a camera (tools/live.bat)");
    const auto cameras = vt::media::listDevices(vt::media::DeviceKind::Video);
    if (cameras.empty()) QSKIP("no video capture device");
    QTemporaryDir dir;
    dir.setAutoRemove(qEnvironmentVariableIsEmpty("VT_APP_LIVE_KEEP"));
    AppSettings settings = testSettings(dir);
    settings.inputs[0] = {};
    settings.inputs[0].kind = vt::media::InputKind::Screen;
    settings.inputs[1] = {};
    settings.inputs[1].kind = vt::media::InputKind::Device;
    settings.inputs[1].videoDevice = cameras.front().name;  // no sound: nothing reaches the speakers
    auto c = std::make_unique<AppController>(settings, Mode::Realtime);
    QTRY_COMPARE_WITH_TIMEOUT(c->loop().inputStatus(0).state, vt::media::InputStatus::State::Running, 10000);
    QTRY_COMPARE_WITH_TIMEOUT(c->loop().inputStatus(1).state, vt::media::InputStatus::State::Running, 10000);

    QElapsedTimer clock;
    clock.start();
    c->startRecording();
    QVERIFY(c->recording());
    QTest::qWait(1000);
    c->autoTransition(vt::Direction::Forward);  // the dissolve
    QTRY_COMPARE_WITH_TIMEOUT(c->loop().switcherState().main, vt::Source::In2, 4000);
    const int wipe = showCrouton(*c, "4000FX/Wipes", "Wipe Diag TL-BR");
    QVERIFY(wipe >= 0);
    c->loadCrouton(wipe, true);  // as a double click: load and Auto
    QTRY_COMPARE_WITH_TIMEOUT(c->loop().switcherState().main, vt::Source::In1, 6000);
    QTest::qWait(int(std::max<qint64>(0, 10000 - clock.elapsed())));

    const int64_t written = c->loop().stats().ticks;
    QElapsedTimer t;
    t.start();
    c->window().close();
    c.reset();
    QVERIFY2(t.elapsed() < 3000, qPrintable(QString::number(t.elapsed())));
    const QStringList found = QDir(settings.recordingPath).entryList({QStringLiteral("vtoast-*")}, QDir::Files);
    QCOMPARE(found.size(), 1);
    const QString file = QDir(settings.recordingPath).filePath(found[0]);
    const int64_t frames = videoFrames(file);
    qInfo("%s: %lld frames, shutdown %lld ms, %lld ticks run", qPrintable(QDir::toNativeSeparators(file)),
          static_cast<long long>(frames), static_cast<long long>(t.elapsed()), static_cast<long long>(written));
    QVERIFY2(frames >= 560 && frames <= 640, qPrintable(QString::number(frames)));  // 10 s = 599.4 frames
}

std::unique_ptr<QObject> makeAppE2eTests() { return std::make_unique<TestAppE2e>(); }

#include "test_app_e2e.moc"
