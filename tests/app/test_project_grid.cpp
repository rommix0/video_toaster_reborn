#include "project_grid.h"
#include "toaster_screen.h"

#include <QComboBox>
#include <QWheelEvent>
#include <QtTest>
#include <filesystem>
#include <memory>

#include "test_support.h"
#include "vt/assets/icons.h"

namespace {

using namespace apptest;
using vt::assets::Crouton;

std::filesystem::path effects() { return std::filesystem::path(VT_DATA_ROOT) / "4.2/Effects"; }
std::filesystem::path data42() { return std::filesystem::path(VT_DATA_ROOT) / "4.2"; }

const std::vector<Crouton>& wipes() {
    static const auto c = vt::assets::scanCroutons(effects() / "4000FX/Wipes");
    return c;
}

const QRgb kLoaded = qRgb(0xff, 0xee, 0x77), kNext = qRgb(0xdd, 0xdd, 0xdd);  // FE7, DDD

// Column col, visible row row of the grid, in screen pixels (SCREEN.md §5).
QRect cell(int col, int row) { return {64 + 80 * col, 44 + 50 * row, 80, 50}; }

QRgb croutonRgb(const Crouton& c, int x, int y) {
    const auto& p = c.palette[c.image.px[size_t(y) * 80 + size_t(x)]];
    return qRgb(p.r, p.g, p.b);
}

// Whether the cell at r of the RGB screen shows crouton c pixel for pixel; names the first difference.
bool shows(const QImage& scr, const QRect& r, const Crouton& c, QString* why) {
    for (int y = 0; y < 50; ++y)
        for (int x = 0; x < 80; ++x)
            if (scr.pixel(r.x() + x, r.y() + y) != croutonRgb(c, x, y)) {
                *why = QStringLiteral("%1: pixel (%2,%3)").arg(QString::fromStdString(c.name)).arg(x).arg(y);
                return false;
            }
    return true;
}

void wheel(QWidget& w, QPoint pos, int deltaY) {
    QWheelEvent e(QPointF(pos), QPointF(w.mapToGlobal(pos)), QPoint(), QPoint(0, deltaY), Qt::NoButton, Qt::NoModifier,
                  Qt::NoScrollPhase, false);
    QApplication::sendEvent(&w, &e);
}

}  // namespace

class TestProjectGrid : public QObject {
    Q_OBJECT
private slots:
    void cellsShowTheCroutonsInTheirOwnColours();
    void clickLoadsDoubleClickRuns();
    void wheelScrollsByRows();
    void loadedBoxAndNextBracket();
    void drawerPicker();
    void framestoreDrawers();
    void cgKeysDrawer();
    void cgScrollAndCrawlDrawers();
    void fitZoomChoice();
    void windowedZoomChoice();
};

// Ruling 3: the indexed screen keeps the panel clear colour under the grid; the RGB screen and the
// display show the croutons in their own CMAP colours, zoom x zoom device pixels each.
void TestProjectGrid::cellsShowTheCroutonsInTheirOwnColours() {
    FakeActions fake;
    ToasterScreen w(layout(), font(), fake);
    w.setCroutons(wipes());
    w.setZoom(2);
    w.resize(w.sizeHint());
    QCOMPARE(w.project().croutons().size(), size_t(79));
    QCOMPARE(w.project().scrollRow(), 0);

    const QImage scr = w.renderRgbForTest();
    QCOMPARE(scr.size(), QSize(768, 481));
    QString why;
    QVERIFY2(shows(scr, cell(2, 1), wipes()[10], &why), qPrintable(why));
    QVERIFY2(shows(scr, cell(0, 0), wipes()[0], &why), qPrintable(why));
    QVERIFY2(shows(scr, cell(7, 3), wipes()[31], &why), qPrintable(why));

    const QImage idx = w.renderIndexedForTest();
    for (int y = 0; y < 481; ++y)
        for (int x = 0; x < 768; ++x) {
            if (QRect(64, 44, 640, 200).contains(x, y))
                QCOMPARE(int(idx.constScanLine(y)[x]), layout().clearColour);
            else if (scr.pixel(x, y) != idx.color(idx.constScanLine(y)[x]))
                QFAIL(qPrintable(QStringLiteral("panel pixel (%1,%2)").arg(x).arg(y)));
        }

    const QImage shown = w.grab().toImage().convertToFormat(QImage::Format_RGB32);
    QCOMPARE(shown.size(), QSize(768 * 2, 481 * 2));
    for (int y = 0; y < shown.height(); ++y)
        for (int x = 0; x < shown.width(); ++x)
            if (shown.pixel(x, y) != scr.pixel(x / 2, y / 2))
                QFAIL(qPrintable(QStringLiteral("device pixel (%1,%2)").arg(x).arg(y)));
}

void TestProjectGrid::clickLoadsDoubleClickRuns() {
    FakeActions fake;
    ToasterScreen w(layout(), font(), fake);
    w.setCroutons(wipes());
    w.setZoom(2);
    w.resize(w.sizeHint());
    w.show();
    QVERIFY(QTest::qWaitForWindowExposed(&w));
    const QPoint at = cell(2, 1).center() * 2;
    QTest::mouseClick(&w, Qt::LeftButton, {}, at);
    QCOMPARE(fake.calls, QStringList{"crouton 10 0"});
    QCOMPARE(w.project().next(), 10);
    QCOMPARE(w.project().loaded(), -1);  // the app sets it from the engine's current effect
    QTest::mouseDClick(&w, Qt::LeftButton, {}, at);
    QTest::mouseRelease(&w, Qt::LeftButton, {}, at);
    QCOMPARE(fake.calls, (QStringList{"crouton 10 0", "crouton 10 1"}));

    // 79 croutons: the last row has 7, so its eighth cell is empty and does nothing.
    fake.calls.clear();
    w.setScrollRow(100);
    QCOMPARE(w.project().scrollRow(), 6);
    QTest::mouseClick(&w, Qt::LeftButton, {}, cell(7, 3).center() * 2);
    QTest::mouseClick(&w, Qt::LeftButton, {}, cell(6, 3).center() * 2);
    QCOMPARE(fake.calls, QStringList{"crouton 78 0"});
}

void TestProjectGrid::wheelScrollsByRows() {
    FakeActions fake;
    ToasterScreen w(layout(), font(), fake);
    w.setCroutons(wipes());
    w.resize(w.sizeHint());
    const QPoint over = cell(3, 2).center();
    wheel(w, over, -120);  // one notch down
    QCOMPARE(w.project().scrollRow(), 1);
    QString why;
    QVERIFY2(shows(w.renderRgbForTest(), cell(2, 0), wipes()[10], &why), qPrintable(why));
    wheel(w, over, -60);  // half notches add up
    QCOMPARE(w.project().scrollRow(), 1);
    wheel(w, over, -60);
    QCOMPARE(w.project().scrollRow(), 2);
    wheel(w, over, -120 * 20);  // clamped to the last full page
    QCOMPARE(w.project().scrollRow(), 6);
    wheel(w, over, 120 * 20);
    QCOMPARE(w.project().scrollRow(), 0);
    wheel(w, centre(node("take").rect), -120);  // the panel does not scroll the grid
    QCOMPARE(w.project().scrollRow(), 0);
    QVERIFY(fake.calls.isEmpty());
}

// My call (the original drawing was not found): loaded = 2-px FE7 frame inside the cell, next =
// 2-px DDD corner brackets with 10-px arms, drawn over the frame.
void TestProjectGrid::loadedBoxAndNextBracket() {
    FakeActions fake;
    ToasterScreen w(layout(), font(), fake);
    w.setCroutons(wipes());
    w.setLoaded(10);
    w.setNext(3);
    const QImage scr = w.renderRgbForTest();
    const QRect box = cell(2, 1), bracket = cell(3, 0);
    for (int x = 0; x < 80; ++x)
        for (int y : {0, 1, 48, 49}) QCOMPARE(scr.pixel(box.x() + x, box.y() + y), kLoaded);
    for (int y = 0; y < 50; ++y)
        for (int x : {0, 1, 78, 79}) QCOMPARE(scr.pixel(box.x() + x, box.y() + y), kLoaded);
    QCOMPARE(scr.pixel(box.x() + 2, box.y() + 2), croutonRgb(wipes()[10], 2, 2));
    for (const QPoint p : {QPoint(0, 0), QPoint(9, 0), QPoint(0, 9), QPoint(1, 1), QPoint(79, 49), QPoint(70, 48),
                           QPoint(78, 40), QPoint(79, 0), QPoint(0, 49)})
        QCOMPARE(scr.pixel(bracket.topLeft() + p), kNext);
    for (const QPoint p : {QPoint(10, 0), QPoint(0, 10), QPoint(40, 0), QPoint(2, 2), QPoint(69, 49)})
        QCOMPARE(scr.pixel(bracket.topLeft() + p), croutonRgb(wipes()[3], p.x(), p.y()));

    w.setLoaded(-1);
    w.setNext(-1);
    QString why;
    QVERIFY2(shows(w.renderRgbForTest(), box, wipes()[10], &why), qPrintable(why));
    QVERIFY2(shows(w.renderRgbForTest(), bracket, wipes()[3], &why), qPrintable(why));
}

// Stand-in drawer picker: the drawers under 4.2/Effects that hold croutons.
void TestProjectGrid::drawerPicker() {
    FakeActions fake;
    auto* screen = new ToasterScreen(layout(), font(), fake);
    ProjectView view(data42(), std::filesystem::path("no/such/folder"), screen);
    QCOMPARE(view.screen(), screen);
    QComboBox* picker = view.drawerPicker();
    const int wipesAt = picker->findText(QStringLiteral("4000FX/Wipes"));
    QVERIFY(wipesAt >= 0);
    QVERIFY(picker->findText(QStringLiteral("ChromaFx")) >= 0);
    QVERIFY(picker->findText(QStringLiteral("2000FX/Animals")) >= 0);
    QVERIFY(picker->findText(QStringLiteral("4000FX")) >= 0);  // "Smooth Fade" sits in 4000FX itself
    for (int i = 0; i < picker->count(); ++i) QVERIFY(!picker->itemText(i).isEmpty() && picker->itemText(i) != ".");
    QCOMPARE(picker->currentIndex(), 0);
    picker->setCurrentIndex(wipesAt);
    QCOMPARE(screen->project().croutons().size(), size_t(79));
    QCOMPARE(screen->project().croutons()[10].name, wipes()[10].name);
    QVERIFY(fake.calls.isEmpty());
}

// After the Effects drawers: Framestore (and its subfolders), CG/Frames, and the saved folder, which
// is listed even when it does not exist and is re-read by rescan() and setSavedFolder().
void TestProjectGrid::framestoreDrawers() {
    FakeActions fake;
    auto* screen = new ToasterScreen(layout(), font(), fake);
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const std::filesystem::path saved = std::filesystem::path(dir.path().toStdU16String()) / "Saved";
    ProjectView view(data42(), saved, screen);
    QComboBox* picker = view.drawerPicker();
    const int wipes = picker->findText(QStringLiteral("4000FX/Wipes")), bars = picker->findText(QStringLiteral("Framestore")),
              cg = picker->findText(QStringLiteral("Framestore/CGDemo")), frames = picker->findText(QStringLiteral("CG/Frames")),
              savedAt = picker->findText(QStringLiteral("Saved framestores"));
    QVERIFY(wipes >= 0 && bars > wipes && cg > bars && frames > cg);
    QCOMPARE(savedAt, picker->count() - 1);
    picker->setCurrentIndex(bars);
    bool found = false;
    for (const auto& c : screen->project().croutons()) found |= c.name == "ColorBars";
    QVERIFY(found);
    picker->setCurrentIndex(savedAt);  // the folder is not there
    QVERIFY(screen->project().croutons().empty());

    // A crouton written into it shows after rescan(); setSavedFolder() points the drawer elsewhere.
    QVERIFY(std::filesystem::create_directories(saved));
    std::filesystem::copy(data42() / "Framestore/ColorBars", saved / "Bars");
    std::filesystem::copy(data42() / "Framestore/ColorBars.I", saved / "Bars.I");
    QVERIFY(screen->project().croutons().empty());
    view.rescan();
    QCOMPARE(screen->project().croutons().size(), size_t(1));
    QCOMPARE(screen->project().croutons()[0].name, std::string("Bars"));
    view.setSavedFolder(data42() / "Framestore");
    QVERIFY(screen->project().croutons().size() > 1);
}

// The CG/Keys drawer, after CG/Frames, lists the six key pages; KeyPage1 has no .i file, so its
// icon is the 80x50 one inside the page file. A click loads, a double click also runs.
void TestProjectGrid::cgKeysDrawer() {
    FakeActions fake;
    auto* screen = new ToasterScreen(layout(), font(), fake);
    ProjectView view(data42(), std::filesystem::path("no/such/folder"), screen);
    QComboBox* picker = view.drawerPicker();
    const int frames = picker->findText(QStringLiteral("CG/Frames")), keys = picker->findText(QStringLiteral("CG/Keys"));
    QVERIFY(frames >= 0 && keys > frames && keys < picker->count() - 1);
    picker->setCurrentIndex(keys);
    QStringList names;
    for (const auto& c : screen->project().croutons()) names << QString::fromStdString(c.name);
    QCOMPARE(names, (QStringList{"4StarCinema", "Feature", "KeyPage1", "KeyPage2", "LowerThird1", "LowerThird2"}));
    const Crouton& kp1 = screen->project().croutons()[2];
    QCOMPARE(kp1.icon, kp1.effect);
    QCOMPARE(kp1.image.w, 80);

    ToasterScreen w(layout(), font(), fake);
    w.setCroutons(screen->project().croutons());
    w.resize(w.sizeHint());
    w.show();
    QVERIFY(QTest::qWaitForWindowExposed(&w));
    const QPoint at = cell(4, 0).center();
    QTest::mouseClick(&w, Qt::LeftButton, {}, at);
    QTest::mouseDClick(&w, Qt::LeftButton, {}, at);
    QTest::mouseRelease(&w, Qt::LeftButton, {}, at);
    QCOMPARE(fake.calls, (QStringList{"crouton 4 0", "crouton 4 1"}));
}

// docs/2026-10-30-cg-scrolls-design.md 2 B: the CG/Scrolls and CG/Crawls drawers follow CG/Keys and list
// the two scroll and the two crawl pages.
void TestProjectGrid::cgScrollAndCrawlDrawers() {
    FakeActions fake;
    auto* screen = new ToasterScreen(layout(), font(), fake);
    ProjectView view(data42(), std::filesystem::path("no/such/folder"), screen);
    QComboBox* picker = view.drawerPicker();
    const int keys = picker->findText(QStringLiteral("CG/Keys")), scrolls = picker->findText(QStringLiteral("CG/Scrolls")),
              crawls = picker->findText(QStringLiteral("CG/Crawls"));
    QVERIFY(keys >= 0 && scrolls == keys + 1 && crawls == scrolls + 1 && crawls < picker->count() - 1);
    auto names = [&](int drawer) {
        picker->setCurrentIndex(drawer);
        QStringList out;
        for (const auto& c : screen->project().croutons()) out << QString::fromStdString(c.name);
        return out;
    };
    QCOMPARE(names(scrolls), (QStringList{"MojoWorld.Scroll", "NightOfTerror"}));
    QCOMPARE(names(crawls), (QStringList{"EBS.Crawl", "Novel.Crawl"}));
}

// Review Focus 1: the largest whole-number zoom whose screen plus 40 px of window chrome fits, and
// with the mixer dock beside it, its width too.
void TestProjectGrid::fitZoomChoice() {
    QCOMPARE(fitZoom(QSize(1366, 728)), 1);
    QCOMPARE(fitZoom(QSize(1920, 1040)), 2);
    QCOMPARE(fitZoom(QSize(3840, 2080)), 4);
    QCOMPARE(fitZoom(QSize(640, 400)), 1);  // never below 1
    QCOMPARE(fitZoom(QSize(1536, 1002)), 2);
    QCOMPARE(fitZoom(QSize(1536, 1001)), 1);
    QCOMPARE(fitZoom(QSize(1366, 728), 40, 0), 1);
    QCOMPARE(fitZoom(QSize(1920, 1040), 40, 0), 2);
    QCOMPARE(fitZoom(QSize(3840, 2080), 40, 0), 4);
    // 1920 x 1200 at 100 %, taskbar 40: zoom 2 is 1536 wide, too wide beside the 508-px dock.
    QCOMPARE(fitZoom(QSize(1920, 1160), 130, 508), 1);
    QCOMPARE(fitZoom(QSize(1920 + 508, 1160), 130, 508), 2);
    // 3840 x 2160 at 100 % with the same chrome: zoom 4 (3072 + 508 wide, 1924 + 130 high) still fits.
    QCOMPARE(fitZoom(QSize(3840, 2120), 130, 508), 4);
    // At 150 % the chrome is 1.5 times as many device pixels: 3.
    QCOMPARE(fitZoom(QSize(3840, 2100), 195, 762), 3);
}

// The windowed default: the device pixel ratio rounded up (about one Toaster pixel per logical
// pixel), capped by the fit, never below 1.
void TestProjectGrid::windowedZoomChoice() {
    QCOMPARE(windowedZoom(4, 1.0), 1);   // 100 %: 768 x 481 on a 4K display
    QCOMPARE(windowedZoom(4, 1.25), 2);
    QCOMPARE(windowedZoom(4, 1.5), 2);   // 4K at 150 %: 1536 x 962 device = 1024 x 641 logical
    QCOMPARE(windowedZoom(4, 2.0), 2);
    QCOMPARE(windowedZoom(4, 2.5), 3);
    QCOMPARE(windowedZoom(1, 1.5), 1);   // the fit still wins on a small display
    QCOMPARE(windowedZoom(0, 1.0), 1);   // never below 1
}

std::unique_ptr<QObject> makeProjectGridTests() { return std::make_unique<TestProjectGrid>(); }

#include "test_project_grid.moc"
