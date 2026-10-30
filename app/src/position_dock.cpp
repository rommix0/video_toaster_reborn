#include "position_dock.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QRadialGradient>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
#include <numbers>

#include "vt/hardware_guesses.h"

Trackball::Trackball(QWidget* parent) : QWidget(parent) {
    setFocusPolicy(Qt::NoFocus);
    setMinimumSize(80, 80);
    setCursor(Qt::OpenHandCursor);
}

// A shaded ball, lit from the top left, with a grid of dots on its surface: the grid scrolls with
// the roll and is wrapped onto the sphere (x = r sin(u / r)), the dots shrinking toward the rim.
void Trackball::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const double r = std::min(width(), height()) / 2.0 - 2;
    const QPointF c = QRectF(rect()).center();
    const bool on = isEnabled();
    QRadialGradient shade(c - QPointF(r / 3, r / 3), r * 1.5);
    shade.setColorAt(0, on ? QColor(240, 215, 140) : QColor(205, 205, 205));
    shade.setColorAt(1, on ? QColor(110, 75, 25) : QColor(105, 105, 105));
    p.setPen(QPen(QColor(40, 40, 40), 1.5));
    p.setBrush(shade);
    p.drawEllipse(c, r, r);

    QPainterPath ball;
    ball.addEllipse(c, r, r);
    p.setClipPath(ball);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(50, 30, 10, on ? 170 : 90));
    const double step = r / 2, reach = r * std::numbers::pi / 2;
    auto wrap = [step](double v) { return std::fmod(std::fmod(v, step) + step, step); };
    for (int i = -4; i <= 4; ++i)
        for (int j = -4; j <= 4; ++j) {
            const double u = wrap(roll_.x()) + i * step, v = wrap(roll_.y()) + j * step;
            if (std::abs(u) > reach || std::abs(v) > reach) continue;
            const double x = r * std::sin(u / r), y = r * std::sin(v / r), d2 = (x * x + y * y) / (r * r);
            if (d2 >= 1) continue;
            const double size = 1 + 3 * std::sqrt(1 - d2);
            p.drawEllipse(c + QPointF(x, y), size, size);
        }
}

void Trackball::mousePressEvent(QMouseEvent* e) {
    if (e->button() != Qt::LeftButton) return QWidget::mousePressEvent(e);
    dragging_ = true;
    last_ = e->position();
    counts_ = {};
    setCursor(Qt::ClosedHandCursor);
}

void Trackball::mouseMoveEvent(QMouseEvent* e) {
    if (!dragging_) return QWidget::mouseMoveEvent(e);
    const QPointF d = e->position() - last_;
    last_ = e->position();
    roll_ += d;
    update();
    counts_ += d * scale_;
    const int dx = int(counts_.x()), dy = int(counts_.y());  // toward 0: the fraction keeps its sign
    counts_ -= QPointF(dx, dy);
    if (dx || dy) emit rolled(dx, dy);
}

void Trackball::mouseReleaseEvent(QMouseEvent* e) {
    if (e->button() != Qt::LeftButton) return QWidget::mouseReleaseEvent(e);
    dragging_ = false;
    setCursor(Qt::OpenHandCursor);
}

void Trackball::changeEvent(QEvent* e) {
    if (e->type() == QEvent::EnabledChange) {  // positioning ended mid-drag: the drag ends too
        dragging_ = false;
        setCursor(Qt::OpenHandCursor);
        update();
    }
    QWidget::changeEvent(e);
}

PositionDock::PositionDock(SwitcherActions& actions, QWidget* parent) : QDockWidget(tr("Position"), parent) {
    setObjectName(QStringLiteral("positionDock"));
    auto* body = new QWidget(this);
    auto* col = new QVBoxLayout(body);
    auto* hint = new QLabel(tr("Release the T-bar midway on a Positionable effect, then roll the ball."), body);
    hint->setWordWrap(true);
    col->addWidget(hint);

    ball_ = new Trackball(body);
    ball_->setCountsPerPixel(vt::HardwareGuesses{}.positionCountsPerPixel);  // the app sets no guesses
    col->addWidget(ball_, 1, Qt::AlignHCenter);
    auto* row = new QHBoxLayout;
    auto* stop = new QPushButton(tr("Stop here"), body);
    auto* finish = new QPushButton(tr("Finish"), body);
    stop->setToolTip(tr("Keep the position and stop the take there (Space, Return)"));
    finish->setToolTip(tr("Keep the position and finish the take"));
    for (QPushButton* b : {stop, finish}) {
        b->setFocusPolicy(Qt::NoFocus);  // Space and Return belong to the Switcher
        row->addWidget(b);
    }
    col->addLayout(row);
    stop_ = stop;
    finish_ = finish;
    setWidget(body);

    connect(ball_, &Trackball::rolled, this, [&actions](int dx, int dy) { actions.movePosition(dx, dy); });
    connect(stop, &QPushButton::clicked, this, [&actions] { actions.endPositioning(vt::PositionEnd::KeepAndStop); });
    connect(finish, &QPushButton::clicked, this, [&actions] { actions.endPositioning(vt::PositionEnd::Finish); });
    setState({});
}

void PositionDock::setState(const vt::media::SwitcherSnapshot& s) {
    for (QWidget* w : {static_cast<QWidget*>(ball_), static_cast<QWidget*>(stop_), static_cast<QWidget*>(finish_)})
        w->setEnabled(s.positioning);
}
