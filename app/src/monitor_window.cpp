#include "monitor_window.h"

#include <QAction>
#include <QActionGroup>
#include <QEvent>
#include <QPainter>
#include <algorithm>
#include <cmath>

#include "pixel_replicate.h"

void MonitorWindow::Mailbox::onPictures(int64_t, const vt::Frame& program, const vt::Frame& preview) {
    const std::lock_guard lock(mu_);
    latest_ = program_ ? program : preview;  // same size: no allocation after the first copy
    fresh_ = true;
}

bool MonitorWindow::Mailbox::take(vt::Frame& into) {
    const std::lock_guard lock(mu_);
    if (!fresh_) return false;
    std::swap(into, latest_);
    fresh_ = false;
    return true;
}

MonitorWindow::MonitorWindow(Kind kind, QWidget* parent)
    : QWidget(parent, Qt::Window), mailbox_(kind == Kind::Program) {
    setWindowTitle(kind == Kind::Program ? QStringLiteral("Program") : QStringLiteral("Preview"));
    setAttribute(Qt::WA_OpaquePaintEvent);

    // The context menu: the widget's own actions.
    setContextMenuPolicy(Qt::ActionsContextMenu);
    auto* group = new QActionGroup(this);
    for (int z = 1; z <= kMaxZoom; ++z) {
        auto* a = addAction(tr("Zoom %1x").arg(z));
        a->setCheckable(true);
        a->setChecked(z == zoom_);
        group->addAction(a);
        connect(a, &QAction::triggered, this, [this, z] { setZoom(z); });
        zoomActions_ << a;
    }
    auto* sep = new QAction(this);
    sep->setSeparator(true);
    addAction(sep);
    underscanAction_ = addAction(tr("Underscan"));
    underscanAction_->setCheckable(true);
    connect(underscanAction_, &QAction::triggered, this, &MonitorWindow::setUnderscan);

    rebuild();
    resize(sizeHint());

    connect(&timer_, &QTimer::timeout, this, &MonitorWindow::poll);
    timer_.start(1000 / 60);
}

vt::Rect MonitorWindow::shownArea() const {
    return underscan_ ? vt::Rect{0, 0, vt::kRasterW, vt::kRasterH} : vt::kVisibleArea;
}

void MonitorWindow::setZoom(int z) {
    z = std::clamp(z, 1, kMaxZoom);
    zoomActions_[z - 1]->setChecked(true);
    if (z == zoom_) return;
    zoom_ = z;
    rebuild();
    resize(sizeHint());
    emit zoomChanged(zoom_);
}

void MonitorWindow::setUnderscan(bool on) {
    underscanAction_->setChecked(on);
    if (on == underscan_) return;
    underscan_ = on;
    rebuild();
    resize(sizeHint());
    emit underscanChanged(underscan_);
}

void MonitorWindow::poll() {
    if (!mailbox_.take(frame_)) return;
    rebuild();
}

void MonitorWindow::rebuild() {
    const vt::Rect a = shownArea();
    const QSize size(a.w * zoom_, a.h * zoom_);
    if (image_.size() != size) image_ = QImage(size, QImage::Format_RGB32);
    replicate(QRect(0, 0, a.w, a.h), zoom_,
              [&](int x, int y) {
                  const vt::Rgba8& p = frame_.at(a.x + x, a.y + y);
                  return qRgb(p.r, p.g, p.b);
              },
              image_);
    update();
}

QSize MonitorWindow::sizeHint() const {
    const vt::Rect a = shownArea();
    const double dpr = devicePixelRatioF();
    return {int(std::ceil(a.w * zoom_ / dpr)), int(std::ceil(a.h * zoom_ / dpr))};
}

bool MonitorWindow::event(QEvent* e) {
    if (e->type() == QEvent::DevicePixelRatioChange) {
        rebuild();
        resize(sizeHint());
    }
    return QWidget::event(e);
}

void MonitorWindow::paintEvent(QPaintEvent*) {
    QPainter p(this);
    // image_ is in device pixels: at the painter's own ratio Qt copies it 1:1, without scaling.
    // The painter's ratio, not the widget's, so that render() onto a device of another ratio is exact too.
    const double dpr = p.deviceTransform().m11();
    image_.setDevicePixelRatio(dpr);
    p.drawImage(QPointF(0, 0), image_);
    const QSizeF shown = image_.deviceIndependentSize();
    if (width() > shown.width()) p.fillRect(QRectF(shown.width(), 0, width() - shown.width(), height()), Qt::black);
    if (height() > shown.height()) p.fillRect(QRectF(0, shown.height(), shown.width(), height() - shown.height()), Qt::black);
}
