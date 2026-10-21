#include "toaster_screen.h"

#include <QVBoxLayout>
#include <QtTest>
#include <cmath>
#include <functional>
#include <memory>
#include <stdexcept>

#include "test_support.h"
#include "vt/assets/font.h"
#include "vt/assets/panel.h"

namespace {

using namespace apptest;
using vt::assets::IndexedImage;
using vt::assets::PanelLayout;
using vt::media::SwitcherSnapshot;

// The region r of the indexed screen as an IndexedImage.
IndexedImage region(const QImage& scr, const vt::Rect& r) {
    IndexedImage out{r.w, r.h, std::vector<uint8_t>(size_t(r.w) * size_t(r.h))};
    for (int y = 0; y < r.h; ++y)
        for (int x = 0; x < r.w; ++x) out.px[size_t(y) * size_t(r.w) + size_t(x)] = scr.constScanLine(r.y + y)[r.x + x];
    return out;
}

void paste(IndexedImage& scr, const vt::Rect& r, const IndexedImage& img) {
    for (int y = 0; y < r.h; ++y)
        for (int x = 0; x < r.w; ++x)
            scr.px[size_t(r.y + y) * size_t(scr.w) + size_t(r.x + x)] = img.px[size_t(y) * size_t(img.w) + size_t(x)];
}

// Compares the whole indexed screen; names the first differing pixel.
bool sameScreen(const QImage& got, const IndexedImage& want, QString* why) {
    if (got.format() != QImage::Format_Indexed8 || got.width() != want.w || got.height() != want.h) {
        *why = QStringLiteral("format/size %1x%2").arg(got.width()).arg(got.height());
        return false;
    }
    for (int y = 0; y < want.h; ++y)
        for (int x = 0; x < want.w; ++x)
            if (got.constScanLine(y)[x] != want.px[size_t(y) * size_t(want.w) + size_t(x)]) {
                *why = QStringLiteral("pixel (%1,%2): %3, want %4").arg(x).arg(y)
                           .arg(got.constScanLine(y)[x]).arg(want.px[size_t(y) * size_t(want.w) + size_t(x)]);
                return false;
            }
    return true;
}

// Ruling 6: renderDefaultScreen(layout, CGInterface 14), from the layout after `tweak`, with the
// selected look of the buttons a default snapshot makes active: Main 1, Preview 2, Superimpose BKG
// and the M speed button.
IndexedImage expectedScreen(const std::function<void(PanelLayout&)>& tweak = {}) {
    PanelLayout l = layout();
    if (tweak) tweak(l);
    IndexedImage want = vt::assets::renderDefaultScreen(l, font());
    for (const char* id : {"main_1", "prev_2", "sup_bkg", "speed_m"})
        paste(want, node(id).rect, vt::assets::selectedLook(node(id), 0));
    return want;
}

// The expected screen with the T-bar showing image `index` (the bottom track cap overlaps it).
IndexedImage withTbarImage(int index) {
    return expectedScreen([index](PanelLayout& l) {
        for (auto& n : l.nodes)
            if (n.id == "tbar") n.image = l.tbarImages[size_t(index)];
    });
}

// The offscreen platform warns when a popup shows; the popup works all the same.
void expectPopupWarnings() {
    if (QGuiApplication::platformName() != QLatin1String("offscreen")) return;
    QTest::ignoreMessage(QtWarningMsg, "This plugin does not support raise()");
    QTest::ignoreMessage(QtWarningMsg, "This plugin does not support grabbing the keyboard");
}

QList<QRgb> colourTable(const vt::assets::Palette& p) {
    QList<QRgb> t;
    for (const auto& c : p) t << qRgb(c.r, c.g, c.b);
    return t;
}

// The widget as shown, in device pixels: every pixel of the panel at `offset` is exactly the zoom x
// zoom block of its Toaster pixel in `scr` (renderRgbForTest), every pixel around it `margin`.
// Names the first pixel that differs.
bool exactPanel(const QImage& shown, const QImage& scr, int zoom, QPoint offset, QRgb margin, QString* why) {
    const QRect panel(offset, scr.size() * zoom);
    for (int y = 0; y < shown.height(); ++y)
        for (int x = 0; x < shown.width(); ++x) {
            const QRgb want =
                panel.contains(x, y) ? scr.pixel((x - offset.x()) / zoom, (y - offset.y()) / zoom) : margin;
            if (shown.pixel(x, y) != want) {
                *why = QStringLiteral("device pixel (%1,%2)").arg(x).arg(y);
                return false;
            }
        }
    return true;
}

}  // namespace

class TestToasterScreen : public QObject {
    Q_OBJECT
private slots:
    void defaultScreenIsTheOriginalWithTheActiveButtonsSelected();
    void clickSelectsTheBusSourceAtZoom2();
    void keyboardMap();
    void tbarImageFollowsTheSnapshot();
    void tbarDragFollowsTheSnapshotNotTheHand();
    void tbarDragDuringAutoFollowsTheSnapshot();
    void freezeIsNotImplemented();
    void displayFollowsTheSnapshotNotTheClick();
    void bkgPopupInTheToasterLook();
    void bkgNameFollowsTheSnapshot();
    void paletteAndExactZoom();
    void rejectsALayoutWithMissingArt();
    void followsADevicePixelRatioChange();
    void snapsToTheLargestZoomThatFits();
    void staysAtZoom1BelowTheOneTimesSize();
};

void TestToasterScreen::defaultScreenIsTheOriginalWithTheActiveButtonsSelected() {
    FakeActions fake;
    ToasterScreen w(layout(), font(), fake);
    w.setState(SwitcherSnapshot{});
    const QImage got = w.renderIndexedForTest();
    QString why;
    QVERIFY2(sameScreen(got, expectedScreen(), &why), qPrintable(why));
    QCOMPARE(got.colorTable(), colourTable(layout().rgbMode));
    QVERIFY(fake.calls.isEmpty());
}

void TestToasterScreen::clickSelectsTheBusSourceAtZoom2() {
    FakeActions fake;
    ToasterScreen w(layout(), font(), fake);
    w.setZoom(2);
    QCOMPARE(w.sizeHint(), QSize(768 * 2, 481 * 2));  // offscreen: devicePixelRatio 1
    w.resize(w.sizeHint());
    w.show();
    QVERIFY(QTest::qWaitForWindowExposed(&w));
    QTest::mouseClick(&w, Qt::LeftButton, {}, centre(node("main_3").hitRect) * 2);
    QCOMPARE(fake.calls, QStringList{"select Main In3"});
}

void TestToasterScreen::keyboardMap() {
    FakeActions fake;
    ToasterScreen w(layout(), font(), fake);
    QCOMPARE(w.focusPolicy(), Qt::StrongFocus);
    QTest::keyClick(&w, Qt::Key_F2);
    QTest::keyClick(&w, Qt::Key_Return);
    QTest::keyClick(&w, Qt::Key_Space, Qt::ShiftModifier);
    QCOMPARE(fake.calls, (QStringList{"select Main In2", "take", "auto Reverse"}));
    fake.calls.clear();
    QTest::keyClick(&w, Qt::Key_Space);
    QTest::keyClick(&w, Qt::Key_Comma);
    QTest::keyClick(&w, Qt::Key_Period);
    QTest::keyClick(&w, Qt::Key_Slash);
    QTest::keyClick(&w, Qt::Key_F7);
    QTest::keyClick(&w, Qt::Key_1);
    QTest::keyClick(&w, Qt::Key_7);
    QTest::keyClick(&w, Qt::Key_Q);
    QTest::keyClick(&w, Qt::Key_U);
    QCOMPARE(fake.calls, (QStringList{"auto Forward", "speed 0", "speed 1", "speed 2", "select Main Bkg",
                                      "select Preview In1", "select Preview Bkg", "select Overlay In1",
                                      "select Overlay Bkg"}));
}

void TestToasterScreen::tbarImageFollowsTheSnapshot() {
    FakeActions fake;
    ToasterScreen w(layout(), font(), fake);
    SwitcherSnapshot s;
    s.tbar = 300;
    w.setState(s);
    QString why;
    QVERIFY2(sameScreen(w.renderIndexedForTest(), withTbarImage(50), &why), qPrintable(why));
    QVERIFY(!sameScreen(w.renderIndexedForTest(), withTbarImage(49), &why));
}

// Ruling 8: the T-bar image always comes from the snapshot; the hand only sends setTBar.
void TestToasterScreen::tbarDragFollowsTheSnapshotNotTheHand() {
    FakeActions fake;
    ToasterScreen w(layout(), font(), fake);
    w.resize(w.sizeHint());
    w.show();
    QVERIFY(QTest::qWaitForWindowExposed(&w));
    const vt::Rect r = layout().tbarRect;
    const QPoint top(r.x + r.w / 2, r.y), bottom(r.x + r.w / 2, r.y + r.h - 1);
    QTest::mousePress(&w, Qt::LeftButton, {}, top);
    QTest::mouseMove(&w, bottom);
    QTest::mouseMove(&w, bottom + QPoint(3, 20));  // still 511: not sent again
    QString why;
    QVERIFY2(sameScreen(w.renderIndexedForTest(), withTbarImage(0), &why), qPrintable(why));
    SwitcherSnapshot s;
    s.tbar = 200;  // the engine has not caught up with the hand yet
    w.setState(s);
    QVERIFY2(sameScreen(w.renderIndexedForTest(), withTbarImage(200 / 6), &why), qPrintable(why));
    QTest::mouseRelease(&w, Qt::LeftButton, {}, bottom);
    QCOMPARE(fake.calls, (QStringList{"tbar 0", "tbar 511"}));
    // After a hand-driven 511 the engine reads 0 again.
    w.setState(SwitcherSnapshot{});
    QVERIFY2(sameScreen(w.renderIndexedForTest(), withTbarImage(0), &why), qPrintable(why));
}

void TestToasterScreen::tbarDragDuringAutoFollowsTheSnapshot() {
    FakeActions fake;
    ToasterScreen w(layout(), font(), fake);
    w.resize(w.sizeHint());
    w.show();
    QVERIFY(QTest::qWaitForWindowExposed(&w));
    SwitcherSnapshot s;
    s.autoRunning = true;
    s.tbar = 400;
    w.setState(s);
    const vt::Rect r = layout().tbarRect;
    QTest::mousePress(&w, Qt::LeftButton, {}, QPoint(r.x + r.w / 2, r.y + 10));
    QTest::mouseMove(&w, QPoint(r.x + r.w / 2, r.y + 20));
    QString why;
    QVERIFY2(sameScreen(w.renderIndexedForTest(), withTbarImage(400 / 6), &why), qPrintable(why));
    s.tbar = 450;
    w.setState(s);
    QVERIFY2(sameScreen(w.renderIndexedForTest(), withTbarImage(450 / 6), &why), qPrintable(why));
    QTest::mouseRelease(&w, Qt::LeftButton, {}, QPoint(r.x + r.w / 2, r.y + 20));
    QCOMPARE(fake.calls.size(), 2);
}

void TestToasterScreen::freezeIsNotImplemented() {
    FakeActions fake;
    ToasterScreen w(layout(), font(), fake);
    w.resize(w.sizeHint());
    w.show();
    QVERIFY(QTest::qWaitForWindowExposed(&w));
    const auto& freeze = node("fs_freeze");
    QTest::mousePress(&w, Qt::LeftButton, {}, centre(freeze.hitRect));
    QCOMPARE(fake.calls, QStringList{"notImplemented FREEZE"});
    // Drawn pressed while held, released afterwards.
    QVERIFY(region(w.renderIndexedForTest(), freeze.rect).px == vt::assets::selectedLook(freeze, 0).px);
    QTest::mouseRelease(&w, Qt::LeftButton, {}, centre(freeze.hitRect));
    QVERIFY(region(w.renderIndexedForTest(), freeze.rect).px == freeze.image.px);
}

void TestToasterScreen::displayFollowsTheSnapshotNotTheClick() {
    FakeActions fake;
    ToasterScreen w(layout(), font(), fake);
    w.resize(w.sizeHint());
    w.show();
    QVERIFY(QTest::qWaitForWindowExposed(&w));
    QTest::mouseClick(&w, Qt::LeftButton, {}, centre(node("main_3").hitRect));
    QCOMPARE(fake.calls, QStringList{"select Main In3"});
    SwitcherSnapshot s;
    s.main = vt::Source::In4;
    w.setState(s);
    const QImage scr = w.renderIndexedForTest();
    QVERIFY(region(scr, node("main_4").rect).px == vt::assets::selectedLook(node("main_4"), 0).px);
    QVERIFY(region(scr, node("main_3").rect).px == node("main_3").image.px);
    QVERIFY(region(scr, node("main_1").rect).px == node("main_1").image.px);
}

// Ruling 9: the colour list is drawn in CGInterface 14 at the screen's zoom.
void TestToasterScreen::bkgPopupInTheToasterLook() {
    FakeActions fake;
    ToasterScreen w(layout(), font(), fake);
    w.setZoom(2);
    w.resize(w.sizeHint());
    w.show();
    QVERIFY(QTest::qWaitForWindowExposed(&w));
    const QPoint bkg = centre(node("bkg_popup").hitRect) * 2;
    expectPopupWarnings();
    QTest::mouseClick(&w, Qt::LeftButton, {}, bkg);
    auto* popup = w.findChild<BkgPopup*>();
    QVERIFY(popup && popup->isVisible());
    QVERIFY(QTest::qWaitForWindowExposed(popup));
    QCOMPARE(popup->zoom(), 2);

    // Every pixel of the "R" of item 3 ("Red") is the CGInterface glyph pixel, in the panel palette.
    const auto& f = font();
    const auto& art = popup->art();
    const QPoint pen = popup->textPen(2);
    const size_t c = size_t('R' - f.loChar);
    const QImage shown = popup->grab().toImage().convertToFormat(QImage::Format_RGB32);
    QCOMPARE(shown.size(), QSize(art.w * 2, art.h * 2));
    const QList<QRgb> pal = colourTable(layout().rgbMode);
    QSet<int> seen;
    for (int gy = 0; gy < f.ySize; ++gy)
        for (int gx = 0; gx < f.charWidth[c]; ++gx) {
            const uint8_t glyph = f.strike.px[size_t(gy) * size_t(f.strike.w) + size_t(f.charLoc[c] + gx)];
            const int x = pen.x() + f.charKern[c] + gx, y = pen.y() - f.baseline + gy;
            QCOMPARE(art.px[size_t(y) * size_t(art.w) + size_t(x)], glyph);
            QCOMPARE(shown.pixel(2 * x + 1, 2 * y + 1), pal[glyph]);
            seen << glyph;
        }
    QVERIFY(seen.size() >= 2);  // strokes and cell background

    // The third item chooses Red and closes the list.
    QTest::mouseClick(popup, Qt::LeftButton, {}, popup->itemRect(2).center() * 2);
    QCOMPARE(fake.calls, QStringList{"bkg 3"});
    QVERIFY(!popup->isVisible());

    // Esc, or a click outside the list, closes it without a choice.
    expectPopupWarnings();
    QTest::mouseClick(&w, Qt::LeftButton, {}, bkg);
    QVERIFY(popup->isVisible());
    QTest::keyClick(popup, Qt::Key_Escape);
    QVERIFY(!popup->isVisible());
    expectPopupWarnings();
    QTest::mouseClick(&w, Qt::LeftButton, {}, bkg);
    QVERIFY(popup->isVisible());
    // A popup gets every click while open: one on the logo reaches it outside its own rect.
    QTest::mouseClick(popup, Qt::LeftButton, {}, popup->mapFromGlobal(w.mapToGlobal(centre(node("logo").rect) * 2)));
    QVERIFY(!popup->isVisible());
    QCOMPARE(fake.calls, QStringList{"bkg 3"});
}

// The pop-up names the snapshot's background, drawn as the original draws "Black".
void TestToasterScreen::bkgNameFollowsTheSnapshot() {
    FakeActions fake;
    ToasterScreen w(layout(), font(), fake);
    SwitcherSnapshot s;
    s.background = vt::EffectColour::of(vt::BkgColour::Magenta);
    w.setState(s);
    const IndexedImage want = expectedScreen([](PanelLayout& l) {
        for (auto& t : l.texts)
            if (t.id == "bkg_popup_text") t.text = l.bkgNames[6];
    });
    QString why;
    QVERIFY2(sameScreen(w.renderIndexedForTest(), want, &why), qPrintable(why));
}

// One Toaster pixel is exactly zoom x zoom device pixels of its palette colour.
void TestToasterScreen::paletteAndExactZoom() {
    FakeActions fake;
    ToasterScreen w(layout(), font(), fake);
    w.setPanelPalette(true);
    w.setZoom(3);
    w.resize(w.sizeHint());
    const QImage scr = w.renderIndexedForTest();
    QCOMPARE(scr.colorTable(), colourTable(layout().previewMode));
    const QImage shown = w.grab().toImage().convertToFormat(QImage::Format_RGB32);
    QCOMPARE(w.zoom(), 3);
    QCOMPARE(shown.size(), QSize(768 * 3, 481 * 3));
    QString why;
    QVERIFY2(exactPanel(shown, scr.convertToFormat(QImage::Format_RGB32), 3, {}, 0, &why), qPrintable(why));
}

// Art the screen would index past is refused up front, with the reason.
void TestToasterScreen::rejectsALayoutWithMissingArt() {
    FakeActions fake;
    PanelLayout few = layout();
    few.tbarImages.resize(85);
    QVERIFY_THROWS_EXCEPTION(std::runtime_error, ToasterScreen(few, font(), fake));
    PanelLayout small = layout();
    for (auto& n : small.nodes)
        if (n.id == "take") n.image = {16, 4, std::vector<uint8_t>(64)};
    QVERIFY_THROWS_EXCEPTION(std::runtime_error, ToasterScreen(small, font(), fake));
}

// Moved to a display at 150 %: sizeHint() becomes 768x481 x 3 / 1.5 logical pixels and the layout
// holding the screen learns it (updateGeometry); until the window takes it, the panel snaps to the
// zoom its old size now fits (4) and an open BKG list follows. At the new size plus a few logical
// pixels every Toaster pixel is exactly 3 x 3 device pixels, centred at a whole device-pixel offset.
void TestToasterScreen::followsADevicePixelRatioChange() {
    FakeActions fake;
    QWidget holder;
    auto* box = new QVBoxLayout(&holder);
    box->setContentsMargins(0, 0, 0, 0);
    auto* w = new WithRatio<ToasterScreen>(layout(), font(), fake);
    box->addWidget(w);
    w->setZoom(3);
    QCOMPARE(holder.sizeHint(), QSize(768 * 3, 481 * 3));
    holder.resize(holder.sizeHint());
    box->activate();  // the minimum size, before the window exists to be told
    holder.show();
    QVERIFY(QTest::qWaitForWindowExposed(&holder));
    expectPopupWarnings();
    QTest::mouseClick(w, Qt::LeftButton, {}, centre(node("bkg_popup").hitRect) * 3);
    auto* popup = w->findChild<BkgPopup*>();
    QVERIFY(popup && popup->isVisible());
    QCOMPARE(popup->size(), QSize(popup->art().w * 3, popup->art().h * 3));

    w->ratio = 1.5;
    // The new size hints: the list's fixed size, and the holder's minimum size (from minimumSizeHint)
    // once its layout runs.
    for (int i = 0; i < 2; ++i)
        QTest::ignoreMessage(QtWarningMsg, "This plugin does not support propagateSizeHints()");
    sendRatioChange(*w);
    box->activate();
    QCOMPARE(holder.minimumSize(), QSize(512, 321));
    QCOMPARE(w->sizeHint(), QSize(768 * 2, 962));
    QCOMPARE(holder.sizeHint(), QSize(768 * 2, 962));
    QCOMPARE(w->zoom(), 4);  // 2304 x 1443 logical = 3456 x 2164 device pixels
    QVERIFY(popup->isVisible());
    QCOMPARE(popup->zoom(), 4);
    QCOMPARE(popup->size(), QSize(int(std::ceil(popup->art().w * 4 / 1.5)), int(std::ceil(popup->art().h * 4 / 1.5))));
    popup->close();

    w->resize(w->sizeHint() + QSize(20, 10));  // 2334 x 1458 device pixels: zoom 3, offset (15, 7)
    QCOMPARE(w->zoom(), 3);
    const QImage scr = w->renderRgbForTest();
    QImage shown(2334, 1458, QImage::Format_RGB32);
    shown.setDevicePixelRatio(1.5);
    shown.fill(Qt::white);
    w->render(&shown);
    shown.setDevicePixelRatio(1.0);
    QString why;
    QVERIFY2(exactPanel(shown, scr, 3, {15, 7}, colourTable(layout().rgbMode)[1], &why), qPrintable(why));
}

// A window larger than the preferred size: the panel takes the largest whole zoom that fits (2),
// centred with margins in the clear colour (index 1); clicks, the T-bar and the BKG list follow the
// offset.
void TestToasterScreen::snapsToTheLargestZoomThatFits() {
    FakeActions fake;
    ToasterScreen w(layout(), font(), fake);
    w.resize(768 * 2 + 50, 481 * 2 + 30);  // offscreen: devicePixelRatio 1
    w.show();
    QVERIFY(QTest::qWaitForWindowExposed(&w));
    QCOMPARE(w.zoom(), 2);
    QCOMPARE(w.sizeHint(), QSize(768, 481));  // the preferred zoom is still 1
    QCOMPARE(layout().clearColour, 1);
    const QPoint offset(25, 15);
    const QImage shown = w.grab().toImage().convertToFormat(QImage::Format_RGB32);
    QCOMPARE(shown.size(), w.size());
    QString why;
    const QRgb clear = colourTable(layout().rgbMode)[1];
    QVERIFY2(exactPanel(shown, w.renderRgbForTest(), 2, offset, clear, &why), qPrintable(why));

    QTest::mouseClick(&w, Qt::LeftButton, {}, offset + centre(node("main_3").hitRect) * 2);
    QTest::mouseClick(&w, Qt::LeftButton, {}, offset - QPoint(1, 1));  // the margin does nothing
    QCOMPARE(fake.calls, QStringList{"select Main In3"});
    const vt::Rect t = layout().tbarRect;
    QTest::mousePress(&w, Qt::LeftButton, {}, offset + QPoint(t.x + t.w / 2, t.y + t.h - 1) * 2);
    QTest::mouseRelease(&w, Qt::LeftButton, {}, offset + QPoint(t.x + t.w / 2, t.y + t.h - 1) * 2);
    QCOMPARE(fake.calls.last(), QStringLiteral("tbar 511"));

    // The list opens above the pop-up, left edges aligned, at the zoom and offset in effect.
    const vt::Rect r = node("bkg_popup").rect;
    expectPopupWarnings();
    QTest::mouseClick(&w, Qt::LeftButton, {}, offset + centre(node("bkg_popup").hitRect) * 2);
    auto* popup = w.findChild<BkgPopup*>();
    QVERIFY(popup && popup->isVisible());
    QCOMPARE(popup->zoom(), 2);
    // Where it was moved (framePosition: the offscreen platform draws a frame even round a popup).
    QCOMPARE(popup->windowHandle()->framePosition(), w.mapToGlobal(offset + QPoint(r.x, r.y - popup->art().h) * 2));
    popup->close();

    // Larger still: zoom 3 once both 768 x 3 and 481 x 3 fit, not before.
    w.resize(768 * 3 + 10, 481 * 3 - 1);
    QCOMPARE(w.zoom(), 2);
    w.resize(768 * 3 + 10, 481 * 3);
    QCOMPARE(w.zoom(), 3);
    const QImage wider = w.grab().toImage().convertToFormat(QImage::Format_RGB32);
    QVERIFY2(exactPanel(wider, w.renderRgbForTest(), 3, {5, 0}, clear, &why), qPrintable(why));
}

// Below 768 x 481 device pixels the zoom stays 1, the panel at the top left; minimumSizeHint() is
// the 1x size, so the layout does not let its window shrink past it.
void TestToasterScreen::staysAtZoom1BelowTheOneTimesSize() {
    FakeActions fake;
    QWidget holder;
    auto* box = new QVBoxLayout(&holder);
    box->setContentsMargins(0, 0, 0, 0);
    auto* w = new ToasterScreen(layout(), font(), fake);
    box->addWidget(w);
    w->setZoom(2);
    QCOMPARE(w->minimumSizeHint(), QSize(768, 481));
    holder.resize(holder.sizeHint());
    box->activate();  // the minimum size, before the window exists to be told
    holder.show();
    QVERIFY(QTest::qWaitForWindowExposed(&holder));
    QCOMPARE(w->zoom(), 2);
    holder.resize(300, 200);
    QTRY_COMPARE(w->size(), QSize(768, 481));
    QCOMPARE(holder.size(), QSize(768, 481));
    QCOMPARE(w->zoom(), 1);

    w->resize(700, 400);  // squeezed anyway: zoom 1, drawn from the top left
    QCOMPARE(w->zoom(), 1);
    QTest::mouseClick(w, Qt::LeftButton, {}, centre(node("main_3").hitRect));
    QCOMPARE(fake.calls, QStringList{"select Main In3"});
    const QImage shown = w->grab().toImage().convertToFormat(QImage::Format_RGB32);
    QString why;
    QVERIFY2(exactPanel(shown, w->renderRgbForTest().copy(0, 0, 700, 400), 1, {}, 0, &why), qPrintable(why));
}

std::unique_ptr<QObject> makeToasterScreenTests() { return std::make_unique<TestToasterScreen>(); }

#include "test_toaster_screen.moc"
