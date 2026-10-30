#include "position_dock.h"

#include <QAbstractButton>
#include <QLabel>
#include <QtTest>
#include <memory>

#include "test_support.h"

using apptest::FakeActions;

class TestPositionDock : public QObject {
    Q_OBJECT
private slots:
    void titleHintAndDisabledUntilPositioning();
    void rollingTheBallSendsMouseCounts();
    void stopHereAndFinish();
};

namespace {

vt::media::SwitcherSnapshot positioning(bool on) {
    vt::media::SwitcherSnapshot s;
    s.positioning = on;
    s.transition = on;
    s.tbar = on ? 256 : 0;
    return s;
}

// A left-button drag over the ball through `points` (QTest events on the widget only).
void roll(Trackball* ball, const QList<QPoint>& points) {
    QTest::mousePress(ball, Qt::LeftButton, {}, points.front());
    for (qsizetype i = 1; i < points.size(); ++i) QTest::mouseMove(ball, points[i]);
    QTest::mouseRelease(ball, Qt::LeftButton, {}, points.back());
}

}  // namespace

// Disabled, and nothing sent, while the Switcher is not positioning; nothing takes the keyboard focus.
void TestPositionDock::titleHintAndDisabledUntilPositioning() {
    FakeActions fake;
    PositionDock dock(fake);
    dock.resize(240, 260);
    dock.show();
    QVERIFY(QTest::qWaitForWindowExposed(&dock));
    QCOMPARE(dock.windowTitle(), QStringLiteral("Position"));
    QCOMPARE(dock.objectName(), QStringLiteral("positionDock"));
    bool hint = false;
    for (QLabel* l : dock.findChildren<QLabel*>())
        hint |= l->text().contains(QStringLiteral("Positionable")) && l->text().contains(QStringLiteral("ball"));
    QVERIFY(hint);
    for (QWidget* w : {static_cast<QWidget*>(dock.ball()), static_cast<QWidget*>(dock.stopButton()),
                       static_cast<QWidget*>(dock.finishButton())}) {
        QVERIFY(!w->isEnabled());
        QCOMPARE(w->focusPolicy(), Qt::NoFocus);
    }
    const QPoint c = dock.ball()->rect().center();
    roll(dock.ball(), {c, c + QPoint(10, 5)});
    dock.stopButton()->click();
    dock.finishButton()->click();
    QVERIFY(fake.calls.isEmpty());
    dock.setState(positioning(true));
    QVERIFY(dock.ball()->isEnabled() && dock.stopButton()->isEnabled() && dock.finishButton()->isEnabled());
    dock.setState(positioning(false));
    QVERIFY(!dock.ball()->isEnabled() && !dock.stopButton()->isEnabled() && !dock.finishButton()->isEnabled());
}

// Logical pixels x the counts-per-pixel scale, right / down +, the fractions kept for the next move;
// only while the left button holds the ball.
void TestPositionDock::rollingTheBallSendsMouseCounts() {
    FakeActions fake;
    PositionDock dock(fake);
    dock.resize(240, 260);
    dock.show();
    QVERIFY(QTest::qWaitForWindowExposed(&dock));
    dock.setState(positioning(true));
    Trackball* ball = dock.ball();
    const QPoint c = ball->rect().center();
    roll(ball, {c, c + QPoint(10, 5), c + QPoint(10, 5), c + QPoint(7, -1)});
    QCOMPARE(fake.calls, (QStringList{"position 10 5", "position -3 -6"}));
    fake.calls.clear();
    QTest::mouseMove(ball, c + QPoint(20, 20));  // no button: nothing
    QVERIFY(fake.calls.isEmpty());
    ball->setCountsPerPixel(0.5);
    roll(ball, {c, c + QPoint(3, 0), c + QPoint(6, -1)});
    QCOMPARE(fake.calls, (QStringList{"position 1 0", "position 2 0"}));  // 1.5 -> 1, then 0.5 + 1.5 -> 2
}

void TestPositionDock::stopHereAndFinish() {
    FakeActions fake;
    PositionDock dock(fake);
    dock.setState(positioning(true));
    QCOMPARE(dock.stopButton()->text(), QStringLiteral("Stop here"));
    QCOMPARE(dock.finishButton()->text(), QStringLiteral("Finish"));
    dock.stopButton()->click();
    dock.finishButton()->click();
    QCOMPARE(fake.calls, (QStringList{"end KeepAndStop", "end Finish"}));
}

std::unique_ptr<QObject> makePositionDockTests() { return std::make_unique<TestPositionDock>(); }

#include "test_position_dock.moc"
