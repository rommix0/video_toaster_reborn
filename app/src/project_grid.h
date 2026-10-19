#pragma once
// The Project grid of the Switcher screen (SCREEN.md §5): the croutons of one drawer, 80x50 icons
// filling an 8 x 4 grid at (64,44) without gaps, scrolled by whole rows. Plus the stand-in drawer
// picker and the default zoom choice.
#include <QImage>
#include <QList>
#include <QRect>
#include <QWidget>
#include <filesystem>
#include <vector>

#include "vt/assets/icons.h"

class QComboBox;
class QScreen;
class ToasterScreen;

// The largest whole-number zoom (device pixels per Toaster pixel, >= 1) with the 768 x 481 screen
// plus chromeHeight of window chrome inside `available`; both in device pixels.
int fitZoom(QSize available, int chromeHeight = 40);
// fitZoom for the screen's available geometry (QScreen::availableGeometry), in its device pixels;
// chromeHeight is in logical pixels.
int defaultZoom(const QScreen&, int chromeHeight = 40);

// What the grid holds and shows. Crouton i sits in slot i: column i % 8, row i / 8; visible rows
// scrollRow()..scrollRow()+3 are drawn at x = 64 + 80 * column, y = 44 + 50 * (row - scrollRow()).
class ProjectGrid {
public:
    static constexpr int kLeft = 64, kTop = 44, kColumns = 8, kRows = 4;  // kRows visible
    static QRect area() { return {kLeft, kTop, kColumns * vt::assets::kCroutonW, kRows * vt::assets::kCroutonH}; }

    ProjectGrid();
    // Back to the top, nothing loaded or next. A crouton whose image is not 80x50 (scanCroutons
    // never gives one) leaves its cell empty.
    void setCroutons(std::vector<vt::assets::Crouton>);
    const std::vector<vt::assets::Crouton>& croutons() const { return croutons_; }
    int rowCount() const;
    int scrollRow() const { return scrollRow_; }
    int loaded() const { return loaded_; }  // -1: none
    int next() const { return next_; }      // -1: none
    // Each returns whether the picture changed; out-of-range values clamp (scroll) or mean none.
    bool setScrollRow(int);  // 0 .. rowCount() - kRows
    bool setLoaded(int);
    bool setNext(int);

    int indexAt(QPoint screenPx) const;  // the crouton shown at that screen pixel, or -1
    // area() in Toaster pixels: each crouton in its own CMAP colours with its highlights;
    // transparent (alpha 0) where no crouton is, so the panel shows through.
    const QImage& image() const { return image_; }

private:
    void compose();

    std::vector<vt::assets::Crouton> croutons_;
    std::vector<QList<QRgb>> colours_;  // per crouton: its CMAP, padded to 256 with black
    int scrollRow_ = 0, loaded_ = -1, next_ = -1;
    QImage image_;
};

// Stand-in for the original drawer browsing (not recreated in v1): a plain combo above the screen
// listing the drawers under <toasterRoot>/4.2/Effects that hold croutons ("4000FX/Wipes"); choosing
// one fills the screen's Project grid. The first drawer is shown at the start.
class ProjectView : public QWidget {
    Q_OBJECT
public:
    ProjectView(const std::filesystem::path& effectsRoot, ToasterScreen* screen, QWidget* parent = nullptr);
    QComboBox* drawerPicker() const { return picker_; }
    ToasterScreen* screen() const { return screen_; }

private:
    void showDrawer(int index);

    QComboBox* picker_;
    ToasterScreen* screen_;
};

// The folders under effectsRoot (any depth) holding at least one effect file with a sibling icon,
// sorted by path (ASCII case-insensitive).
std::vector<std::filesystem::path> croutonDrawers(const std::filesystem::path& effectsRoot);
