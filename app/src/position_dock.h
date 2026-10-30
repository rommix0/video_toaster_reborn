#pragma once
// The Position dock: the on-screen trackball that positions the Positionable effects
// (tools/re/POSITION.md) while the Switcher is positioning (SwitcherSnapshot::positioning). Rolling
// the ball sends its drag as mouse counts (logical pixels x HardwareGuesses::positionCountsPerPixel,
// right / down +) to SwitcherActions::movePosition; "Stop here" keeps the position and sets the stop
// (as Space / Return), "Finish" keeps it and ends the take. Everything is disabled, and nothing is
// sent, while the Switcher is not positioning. Nothing takes keyboard focus (Space and Return belong
// to the Switcher).
#include <QDockWidget>
#include <QPointF>
#include <QWidget>

#include "controller.h"
#include "vt/media/monitor.h"

class QAbstractButton;

// A shaded ball whose surface marks roll with a left-button drag. No momentum.
class Trackball : public QWidget {
    Q_OBJECT
public:
    explicit Trackball(QWidget* parent = nullptr);
    void setCountsPerPixel(double c) { scale_ = c; }
    QSize sizeHint() const override { return {120, 120}; }

signals:
    void rolled(int dx, int dy);  // mouse counts; the fractions wait for the next move

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void changeEvent(QEvent*) override;

private:
    double scale_ = 1.0;
    bool dragging_ = false;
    QPointF last_, counts_;
    QPointF roll_;  // how far the surface has rolled, for the marks
};

class PositionDock : public QDockWidget {
    Q_OBJECT
public:
    explicit PositionDock(SwitcherActions&, QWidget* parent = nullptr);
    void setState(const vt::media::SwitcherSnapshot&);  // enabled only while positioning

    Trackball* ball() const { return ball_; }
    QAbstractButton* stopButton() const { return stop_; }
    QAbstractButton* finishButton() const { return finish_; }

private:
    Trackball* ball_ = nullptr;
    QAbstractButton* stop_ = nullptr;
    QAbstractButton* finish_ = nullptr;
};
