#include "monitor_window.h"

#include "test_support.h"

#include <QAction>
#include <QtTest>
#include <memory>

namespace {

constexpr QRgb kA = qRgb(200, 30, 40), kB = qRgb(20, 60, 220);

// A frame of two colours in a pixel checkerboard, so any off-by-one or smoothing shows up.
vt::Frame checker(bool flip = false) {
    vt::Frame f;
    for (int y = 0; y < vt::kRasterH; ++y)
        for (int x = 0; x < vt::kRasterW; ++x) {
            const QRgb c = ((x + y) & 1) != flip ? kA : kB;
            f.at(x, y) = {uint8_t(qRed(c)), uint8_t(qGreen(c)), uint8_t(qBlue(c)), 255};
        }
    return f;
}

vt::Frame flat(QRgb c) {
    vt::Frame f;
    for (auto& p : f.px) p = {uint8_t(qRed(c)), uint8_t(qGreen(c)), uint8_t(qBlue(c)), 255};
    return f;
}

// Every device pixel (X, Y) of `shown` must be picture pixel (area.x + X / zoom, area.y + Y / zoom) of `f`.
QString firstMismatch(const QImage& shown, const vt::Frame& f, const vt::Rect& area, int zoom) {
    if (shown.size() != QSize(area.w * zoom, area.h * zoom))
        return QStringLiteral("size %1x%2").arg(shown.width()).arg(shown.height());
    for (int y = 0; y < shown.height(); ++y)
        for (int x = 0; x < shown.width(); ++x) {
            const vt::Rgba8& p = f.at(area.x + x / zoom, area.y + y / zoom);
            if ((shown.pixel(x, y) & 0xffffff) != (qRgb(p.r, p.g, p.b) & 0xffffff))
                return QStringLiteral("device pixel (%1,%2)").arg(x).arg(y);
        }
    return {};
}

}  // namespace

class TestMonitor : public QObject {
    Q_OBJECT
private slots:
    void titles();
    void exactZoomAtDeviceRatio1_5();
    void visibleAreaAndUnderscan();
    void previewWindowShowsThePreviewPicture();
    void keepsOnlyTheLatestPicture();
    void showsEachPictureAsItArrives();
    void contextMenuChoices();
    void resizesOnADevicePixelRatioChange();
};

void TestMonitor::titles() {
    MonitorWindow program(MonitorWindow::Kind::Program), preview(MonitorWindow::Kind::Preview);
    QCOMPARE(program.windowTitle(), QStringLiteral("Program"));
    QCOMPARE(preview.windowTitle(), QStringLiteral("Preview"));
}

// Zoom 3 on a display at 150%: one picture pixel is exactly 3 x 3 device pixels (the zoom is device
// pixels per picture pixel), and the window is 1280 x 864 logical pixels = 1920 x 1296 device pixels.
void TestMonitor::exactZoomAtDeviceRatio1_5() {
    MonitorWindow w(MonitorWindow::Kind::Program);
    w.setZoom(3);
    const vt::Frame f = checker();
    w.sink().onPictures(0, f, flat(kA));
    w.poll();
    w.resize(1280, 864);

    QImage shown(1920, 1296, QImage::Format_RGB32);
    shown.setDevicePixelRatio(1.5);
    shown.fill(Qt::white);
    w.render(&shown);
    shown.setDevicePixelRatio(1.0);
    const QString bad = firstMismatch(shown, f, vt::kVisibleArea, 3);
    QVERIFY2(bad.isEmpty(), qPrintable(bad));
}

void TestMonitor::visibleAreaAndUnderscan() {
    MonitorWindow w(MonitorWindow::Kind::Program);
    const vt::Frame f = checker();
    w.sink().onPictures(0, f, f);
    w.poll();
    QCOMPARE(w.shownArea(), vt::kVisibleArea);
    QCOMPARE(w.sizeHint(), QSize(640, 432));  // offscreen: devicePixelRatio 1
    w.show();
    QCOMPARE(w.size(), QSize(640, 432));
    QImage shown = w.grab().toImage().convertToFormat(QImage::Format_RGB32);
    QVERIFY2(firstMismatch(shown, f, vt::kVisibleArea, 1).isEmpty(), "visible area");

    QSignalSpy spy(&w, &MonitorWindow::underscanChanged);
    w.setUnderscan(true);
    QCOMPARE(spy.count(), 1);
    QCOMPARE(w.shownArea(), (vt::Rect{0, 0, 768, 482}));
    QCOMPARE(w.sizeHint(), QSize(768, 482));
    QCOMPARE(w.size(), QSize(768, 482));
    shown = w.grab().toImage().convertToFormat(QImage::Format_RGB32);
    QVERIFY2(firstMismatch(shown, f, {0, 0, 768, 482}, 1).isEmpty(), "underscan");

    w.setZoom(2);  // underscan at zoom 2
    QCOMPARE(w.size(), QSize(768 * 2, 482 * 2));
    shown = w.grab().toImage().convertToFormat(QImage::Format_RGB32);
    QVERIFY2(firstMismatch(shown, f, {0, 0, 768, 482}, 2).isEmpty(), "underscan zoom 2");
}

void TestMonitor::previewWindowShowsThePreviewPicture() {
    MonitorWindow preview(MonitorWindow::Kind::Preview);
    const vt::Frame program = flat(kA), pre = checker(true);
    preview.sink().onPictures(0, program, pre);
    preview.poll();
    preview.show();
    const QImage shown = preview.grab().toImage().convertToFormat(QImage::Format_RGB32);
    QVERIFY2(firstMismatch(shown, pre, vt::kVisibleArea, 1).isEmpty(), "preview picture");
}

// The sink keeps one frame, the newest; poll() shows it, and shows nothing new until another arrives.
void TestMonitor::keepsOnlyTheLatestPicture() {
    MonitorWindow w(MonitorWindow::Kind::Program);
    for (QRgb c : {kA, kB, qRgb(1, 2, 3)}) w.sink().onPictures(0, flat(c), flat(c));
    w.poll();
    w.show();
    QCOMPARE(w.grab().toImage().pixel(10, 10) & 0xffffff, qRgb(1, 2, 3) & 0xffffffu);
    w.sink().onPictures(1, flat(kB), flat(kB));
    w.poll();
    QCOMPARE(w.grab().toImage().pixel(10, 10) & 0xffffff, kB & 0xffffffu);
}

// Each picture is shown when it arrives, not when a timer next looks: a 60 Hz timer beside the 59.94 Hz
// loop dropped about one picture every two seconds. sendPostedEvents delivers queued calls, no timers.
void TestMonitor::showsEachPictureAsItArrives() {
    MonitorWindow w(MonitorWindow::Kind::Program);
    w.show();
    QVERIFY(QTest::qWaitForWindowExposed(&w));
    for (QRgb c : {kA, kB, qRgb(1, 2, 3)}) {
        w.sink().onPictures(0, flat(c), flat(c));
        QCoreApplication::sendPostedEvents();
        QCOMPARE(w.grab().toImage().pixel(10, 10) & 0xffffff, c & 0xffffffu);
    }
}

// The context menu is the window's actions: Zoom 1x..4x and Underscan.
void TestMonitor::contextMenuChoices() {
    MonitorWindow w(MonitorWindow::Kind::Program);
    QSignalSpy zoomSpy(&w, &MonitorWindow::zoomChanged);
    QStringList names;
    QAction* zoom4 = nullptr;
    QAction* underscan = nullptr;
    for (QAction* a : w.actions()) {
        names << a->text();
        if (a->text() == QStringLiteral("Zoom 4x")) zoom4 = a;
        if (a->text() == QStringLiteral("Underscan")) underscan = a;
    }
    QCOMPARE(names, (QStringList{"Zoom 1x", "Zoom 2x", "Zoom 3x", "Zoom 4x", "", "Underscan"}));
    zoom4->trigger();
    QCOMPARE(w.zoom(), 4);
    QCOMPARE(zoomSpy.count(), 1);
    underscan->trigger();
    QVERIFY(w.underscan());
    w.setZoom(9);  // clamped
    QCOMPARE(w.zoom(), 4);
    w.setZoom(0);
    QCOMPARE(w.zoom(), 1);
}

// Moved to a display at 150 %: the window takes its new sizeHint (zoom 3 = 1280 x 864 logical pixels
// there), and every picture pixel is still exactly 3 x 3 device pixels.
void TestMonitor::resizesOnADevicePixelRatioChange() {
    apptest::WithRatio<MonitorWindow> w(MonitorWindow::Kind::Program);
    w.setZoom(3);
    const vt::Frame f = checker();
    w.sink().onPictures(0, f, flat(kA));
    w.poll();
    QCOMPARE(w.size(), QSize(1920, 1296));
    w.ratio = 1.5;
    apptest::sendRatioChange(w);
    QCOMPARE(w.sizeHint(), QSize(1280, 864));
    QCOMPARE(w.size(), QSize(1280, 864));

    QImage shown(1920, 1296, QImage::Format_RGB32);
    shown.setDevicePixelRatio(1.5);
    shown.fill(Qt::white);
    w.render(&shown);
    shown.setDevicePixelRatio(1.0);
    const QString bad = firstMismatch(shown, f, vt::kVisibleArea, 3);
    QVERIFY2(bad.isEmpty(), qPrintable(bad));
}

std::unique_ptr<QObject> makeMonitorTests() { return std::make_unique<TestMonitor>(); }

#include "test_monitor.moc"
