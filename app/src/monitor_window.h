#pragma once
// The Program and Preview monitors: separate top-level windows standing in for the Toaster's
// external monitors. A window shows the engine's 768 x 482 picture cropped to the visible area
// (640 x 432) or, with underscan, whole, every picture pixel exactly zoom x zoom device pixels
// (zoom 1-4, the same replicate() as the Switcher screen).
//
// Pictures come from the loop thread through sink(): onPictures only copies the frame this
// window shows under a short mutex and returns (the loop holds its own lock across the call). A
// frame that finds the mailbox empty queues one poll() on the UI thread, so every frame is shown as
// it arrives (a timer beside the 59.94 Hz loop dropped some); frames that come while the UI is busy
// replace each other, and the newest is shown.
// The context menu has the zoom and underscan choices. When the window's device pixel ratio changes
// (moved to a display with another scaling) it resizes to its new sizeHint(), so the picture stays
// exactly zoom x zoom device pixels per picture pixel and fills the window.
#include <QImage>
#include <QWidget>
#include <mutex>

#include "vt/media/monitor.h"

class MonitorWindow : public QWidget {
    Q_OBJECT
public:
    enum class Kind { Program, Preview };
    static constexpr int kMaxZoom = 4;

    explicit MonitorWindow(Kind, QWidget* parent = nullptr);

    // For ProgramLoop::addMonitor / removeMonitor.
    vt::media::MonitorSink& sink() { return mailbox_; }

    // Zoom = device pixels per picture pixel, 1..kMaxZoom (others are clamped). sizeHint() is the
    // shown area x zoom in device pixels; a zoom or underscan change resizes the window to it.
    void setZoom(int);
    int zoom() const { return zoom_; }
    void setUnderscan(bool);
    bool underscan() const { return underscan_; }
    vt::Rect shownArea() const;  // kVisibleArea, or the whole raster with underscan

    // Takes the newest frame, if a new one arrived since the last call, and repaints. Queued by the sink.
    void poll();

    QSize sizeHint() const override;

signals:
    void zoomChanged(int);
    void underscanChanged(bool);

protected:
    void paintEvent(QPaintEvent*) override;
    bool event(QEvent*) override;  // DevicePixelRatioChange

private:
    // The one frame the loop thread leaves for the UI thread.
    class Mailbox : public vt::media::MonitorSink {
    public:
        Mailbox(bool program, MonitorWindow* owner) : program_(program), owner_(owner) {}
        void onPictures(int64_t tick, const vt::Frame& program, const vt::Frame& preview) override;
        bool take(vt::Frame& into);  // swaps the newest frame into `into`; false if none is new
    private:
        const bool program_;
        MonitorWindow* const owner_;  // gets the queued poll(); removeMonitor before it goes
        std::mutex mu_;
        vt::Frame latest_;
        bool fresh_ = false;
    };

    void rebuild();  // image_ from frame_ at the current zoom and area, then repaint

    Mailbox mailbox_;
    vt::Frame frame_;      // what image_ shows (black until the first picture)
    QImage image_;         // device pixels, zoom_ x zoom_ per picture pixel
    int zoom_ = 1;
    bool underscan_ = false;
    QList<QAction*> zoomActions_;
    QAction* underscanAction_ = nullptr;
};
