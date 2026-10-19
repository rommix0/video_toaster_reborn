// The app end to end: AppController in Mode::Offline with the Plan 2 file fixtures as inputs, driven
// through the Switcher screen, the menus and the settings dialog.
#include "controller.h"

#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QElapsedTimer>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QStatusBar>
#include <QTemporaryDir>
#include <QtTest>
#include <memory>
#include <thread>

#include "main_window.h"
#include "monitor_window.h"
#include "project_grid.h"
#include "test_support.h"
#include "toaster_screen.h"
#include "vt/effect.h"
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

struct LastPicture : vt::media::MonitorSink {
    vt::Frame program;
    void onPictures(int64_t, const vt::Frame& p, const vt::Frame&) override { program = p; }
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
    void invalidDataRootThenChosenInSettings();
    void recordingIsFinishedWhenTheWindowCloses();
    void stoppingARecordingDoesNotBlock();
    void settingsChangeTheInputs();
    void logLinesFromOtherThreads();
    void menus();
    void realtimeRecordingWithLiveInputs();  // live: tools/live.bat (VT_APP_LIVE=1)
};

// Crouton "Crop To Center" (4000FX/Wipes) is an ALGO move the engine cannot play yet: clicking it
// shows why in the status bar. Clicking "Wipe Diag TL-BR" from the same drawer and then AUTO runs that
// wipe from In1 to In2.
void TestAppE2e::croutonThenAutoRunsTheEffect() {
    QTemporaryDir dir;
    AppSettings settings = testSettings(dir);
    AppController c(settings, Mode::Offline);
    QVERIFY(c.screen());
    const int crop = showCrouton(c, "4000FX/Wipes", "Crop To Center");
    QVERIFY(crop >= 0);
    QTest::mouseClick(c.screen(), Qt::LeftButton, {}, cellCentre(*c.screen(), crop));
    const QString why = c.window().statusBar()->currentMessage();
    QVERIFY2(why.startsWith(QStringLiteral("Cannot load Crop To Center: ")) && why.contains(QStringLiteral("ALGO")),
             qPrintable(why));

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
