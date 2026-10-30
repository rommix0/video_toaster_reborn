#pragma once
// The Video Toaster 4.2 Switcher screen (768 x 481), drawn pixel for pixel from the original panel
// art of switcher.main (../tools/re/SCREEN.md): bus buttons, TAKE, AUTO, S/M/F, the BKG pop-up, the
// T-bar and the framestore / superimpose / key panels, with the Project grid of crouton icons above
// them. Mouse and keys go to SwitcherActions; what the panel shows comes only from the snapshots
// given to setState().
//
// The T-bar is dragged with the left button on it or the right button anywhere on the screen
// (switcher.main 0x1123e), by the same Toaster-pixel scale; letting go sends releaseTBar, and so
// does losing the window's focus or hiding the screen during a drag. While the snapshot says
// `positioning` (a Positionable effect, tools/re/POSITION.md; the Position dock's trackball moves
// it) Space and Return keep the position (KeepAndStop), and a T-bar press first goes back to the
// T-bar with the position kept (BackToTBar). The pointer is never captured.
#include <QImage>
#include <QList>
#include <QRegion>
#include <QWidget>
#include <functional>
#include <string>
#include <vector>

#include "controller.h"
#include "project_grid.h"
#include "vt/assets/font.h"
#include "vt/assets/panel.h"
#include "vt/media/monitor.h"

// The BKG colour list opened from the BKG pop-up. The original list's art and behaviour were not
// found in switcher.main, so this look is a guess: the nine bkgNames (trimmed) in CGInterface 14,
// JAM2 with background pen 0 like the pop-up's own text, on a box bevelled like the panel buttons
// (colour-0 outline, 2-px colour-3 light top/left, 2-px colour-1 shadow bottom/right, colour-2 face),
// shown at the screen's zoom with the same exact pixel replication. A click on a name chooses it;
// a click outside or Esc closes the list.
class BkgPopup : public QWidget {
    Q_OBJECT
public:
    BkgPopup(const vt::assets::ColorFont& cgInterface14, const std::vector<std::string>& names, int minWidth,
             std::function<void(vt::BkgColour)> chosen, QWidget* parent);
    void open(const QPoint& globalTopLeft, int zoom, const QList<QRgb>& colours);
    int zoom() const { return zoom_; }
    const vt::assets::IndexedImage& art() const { return art_; }  // the list in Toaster pixels
    QRect itemRect(int i) const;                                   // item i in art() pixels
    QPoint textPen(int i) const;                                   // item i's text baseline pen in art()

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void keyPressEvent(QKeyEvent*) override;

private:
    const vt::assets::ColorFont& font_;
    int count_ = 0, itemH_ = 0;
    std::function<void(vt::BkgColour)> chosen_;
    vt::assets::IndexedImage art_;
    int zoom_ = 1;
    QImage scaled_;
};

class ToasterScreen : public QWidget {
    Q_OBJECT
public:
    // Throws std::runtime_error naming the node when a drawn node's art does not fill its rect or
    // the T-bar has fewer than 86 images.
    ToasterScreen(const vt::assets::PanelLayout&, const vt::assets::ColorFont& cgInterface14,
                  SwitcherActions&, QWidget* parent = nullptr);

    // Zoom = device pixels per Toaster pixel (whole numbers >= 1), so one Toaster pixel is exactly
    // zoom x zoom device pixels at any devicePixelRatio. The widget takes any size from 768x481
    // device pixels (minimumSizeHint) up: the zoom in effect is the largest that fits it, and the
    // panel is drawn centred at a whole device-pixel offset, the margins around it in the panel clear
    // colour (palette index 1). setZoom sets the preferred zoom: sizeHint() is 768x481 x that zoom
    // device pixels. A change of the device pixel ratio (another display) updates the geometry, snaps
    // the zoom anew and moves an open BKG list along.
    void setZoom(int);
    int zoom() const { return zoom_; }  // the zoom in effect
    void setPanelPalette(bool previewMode);             // false: RGB mode 000 555 999 DDD
    void setState(const vt::media::SwitcherSnapshot&);  // repaints only what changed
    // The Project grid: a single click on a crouton makes it "next" and loads it, a double click
    // also runs Auto, the wheel scrolls by rows. "Loaded" is the crouton of the engine's current
    // effect, which only the app knows.
    void setCroutons(std::vector<vt::assets::Crouton>);  // back to the top, nothing loaded or next
    void setScrollRow(int);
    void setLoaded(int index);                           // -1: none
    void setNext(int index);                             // -1: none
    const ProjectGrid& project() const { return project_; }

    // The Switcher's key map (Global Constraints) for a key press, also for presses AppController
    // forwards from its other windows: true (and accepted) if the key is mapped. Auto-repeats of a
    // mapped key are swallowed.
    bool switcherKey(QKeyEvent*);

    // Ruling 3: the panel as indices, with the Project area left in the clear colour (Format_Indexed8).
    QImage renderIndexedForTest() const;
    // The 768x481 screen as shown, one pixel per Toaster pixel (Format_RGB32): the panel in its
    // palette and the croutons in their own CMAP colours.
    QImage renderRgbForTest() const;

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

protected:
    void paintEvent(QPaintEvent*) override;
    void resizeEvent(QResizeEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void mouseDoubleClickEvent(QMouseEvent*) override;
    void wheelEvent(QWheelEvent*) override;
    bool event(QEvent*) override;  // tool tips: a crouton's name; DevicePixelRatioChange
    void keyPressEvent(QKeyEvent*) override;

private:
    // A clickable node and what it does.
    struct Control {
        enum Kind { Bus, Take, Auto, Speed, Bkg, Freeze, Live, Grab, Save, Key, KeyLevel, Stub } kind;
        int node;
        vt::Bus bus = vt::Bus::Main;
        vt::Source source = vt::Source::In1;
        vt::Speed speed = vt::Speed::Medium;
        int input = 0;  // Grab: 0..3
        QString name;  // for Stub: what notImplemented() reports
        vt::KeyMode keyMode = vt::KeyMode::Off;  // Key
    };

    int nodeIndex(const char* id) const;
    int lookOf(int node) const;            // T-bar: image index; others: 1 = selected look, 0 = normal
    const vt::assets::IndexedImage& imageOf(int node) const;
    std::string bkgText() const;
    std::string levelText() const;         // the clip level as three digits
    QRect textRect(int text, const std::string&) const;
    void refresh();                        // brings looks and texts up to date, redraws what changed
    void redraw(const QRegion&);           // recomposes the indexed screen there and rescales it
    void compose(const QRect&);
    QRgb rgbAt(int x, int y) const;        // the composed screen: panel, or crouton where one is
    void scale(const QRect&);              // composed -> device pixels (the one place zoom is applied)
    void projectChanged(bool changed);     // redraws the Project area when the grid's picture changed
    void rescaleAll();
    bool snap();                           // the zoom and offset in effect for the widget's size
    QSize sizeAt(int zoom) const;          // logical pixels covering the screen at that zoom
    QRect logicalRect(const QRect& screenRect) const;  // widget pixels covering a screen rect
    QPointF toScreen(const QPointF& logical) const;
    QPoint toScreenPixel(const QPointF& logical) const;
    int tbarFor(double screenY) const;
    void grabTbar();                       // a T-bar press while positioning: BackToTBar first
    void startRightDrag(double screenY);
    void endDrag();                        // a T-bar drag let go: releaseTBar
    void openBkgPopup();

    vt::assets::PanelLayout layout_;
    vt::assets::ColorFont font_;
    SwitcherActions& actions_;
    std::vector<int> drawn_;                         // visible nodes, in draw order
    std::vector<int> look_;                          // per node, as last drawn (-1 = not drawn)
    std::vector<vt::assets::IndexedImage> selected_; // per node, its selected look (empty if none)
    std::vector<Control> controls_;
    int tbarNode_ = -1, bkgNode_ = -1, bkgText_ = -1, levelNode_ = -1, levelText_ = -1;
    vt::media::SwitcherSnapshot state_;
    int dragLevel_ = -1;                             // level box drag: the last level sent, -1 = none
    int levelAtPress_ = 0;
    double pressX_ = 0;                              // the press, in Toaster pixels
    int dragTbar_ = -1;                              // last setTBar of a drag (only to skip repeats)
    bool rightDrag_ = false;                         // the right button drags the T-bar
    int tbarAtPress_ = 0;                            // right drag: the T-bar when it began
    double pressY_ = 0;                              // right drag: where it began, in Toaster pixels
    int pressed_ = -1;                               // node drawn pressed while the mouse holds it
    int preferredZoom_ = 1;                          // setZoom: sizeHint()
    int zoom_ = 1;                                   // in effect: the largest that fits the widget
    QPoint offset_;                                  // the panel's top left in the widget, device pixels
    QList<QRgb> colours_;
    vt::assets::IndexedImage screen_;                // the indexed screen
    ProjectGrid project_;                            // drawn over screen_ in its own colours (Ruling 3)
    int wheelAngle_ = 0;                             // wheel turn not yet worth a row (1/8 degrees)
    QImage scaled_;                                  // the composed screen in device pixels, zoom_ x zoom_ each
    BkgPopup* bkgPopup_ = nullptr;
};
