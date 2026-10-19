#pragma once
// The Video Toaster 4.2 Switcher screen (768 x 481), drawn pixel for pixel from the original panel
// art of switcher.main (../tools/re/SCREEN.md): bus buttons, TAKE, AUTO, S/M/F, the BKG pop-up, the
// T-bar and the framestore / superimpose / key panels. Mouse and keys go to SwitcherActions; what
// the screen shows comes only from the snapshots given to setState().
#include <QImage>
#include <QList>
#include <QRegion>
#include <QWidget>
#include <string>
#include <vector>

#include "controller.h"
#include "vt/assets/font.h"
#include "vt/assets/panel.h"
#include "vt/media/monitor.h"

class QMenu;

class ToasterScreen : public QWidget {
    Q_OBJECT
public:
    ToasterScreen(const vt::assets::PanelLayout&, const vt::assets::ColorFont& cgInterface14,
                  SwitcherActions&, QWidget* parent = nullptr);

    // Zoom = device pixels per Toaster pixel (whole numbers >= 1), so one Toaster pixel is exactly
    // zoom x zoom device pixels at any devicePixelRatio; sizeHint() is 768x481 x zoom device pixels.
    void setZoom(int);
    int zoom() const { return zoom_; }
    void setPanelPalette(bool previewMode);             // false: RGB mode 000 555 999 DDD
    void setState(const vt::media::SwitcherSnapshot&);  // repaints only what changed
    QImage renderIndexedForTest() const;                // the current 768x481 screen as indices (Format_Indexed8)

    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void keyPressEvent(QKeyEvent*) override;

private:
    // A clickable node and what it does.
    struct Control {
        enum Kind { Bus, Take, Auto, Speed, Bkg, Stub } kind;
        int node;
        vt::Bus bus = vt::Bus::Main;
        vt::Source source = vt::Source::In1;
        vt::Speed speed = vt::Speed::Medium;
        QString name;  // for Stub: what notImplemented() reports
    };

    int nodeIndex(const char* id) const;
    int lookOf(int node) const;            // T-bar: image index; others: 1 = selected look, 0 = normal
    const vt::assets::IndexedImage& imageOf(int node) const;
    std::string bkgText() const;
    QRect bkgTextRect(const std::string&) const;
    void refresh();                        // brings looks and texts up to date, redraws what changed
    void redraw(const QRegion&);           // recomposes the indexed screen there and rescales it
    void compose(const QRect&);
    void scale(const QRect&);              // indexed -> device pixels (the one place zoom is applied)
    void rescaleAll();
    QPointF toScreen(const QPointF& logical) const;
    int tbarFor(double screenY) const;
    void openBkgMenu();

    vt::assets::PanelLayout layout_;
    vt::assets::ColorFont font_;
    SwitcherActions& actions_;
    std::vector<int> drawn_;                         // visible nodes, in draw order
    std::vector<int> look_;                          // per node, as last drawn (-1 = not drawn)
    std::vector<vt::assets::IndexedImage> selected_; // per node, its selected look (empty if none)
    std::vector<Control> controls_;
    int tbarNode_ = -1, bkgNode_ = -1, bkgText_ = -1;
    vt::media::SwitcherSnapshot state_;
    int dragTbar_ = -1;                              // the hand's T-bar position while dragging
    int pressed_ = -1;                               // node drawn pressed while the mouse holds it
    int zoom_ = 1;
    QList<QRgb> colours_;
    vt::assets::IndexedImage screen_;                // the indexed screen
    QImage scaled_;                                  // screen_ in device pixels, zoom_ x zoom_ each
    QMenu* bkgMenu_ = nullptr;
};
