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
// plus chromeHeight of window chrome above and below it and chromeWidth beside it (the mixer dock)
// inside `available`; all in device pixels.
int fitZoom(QSize available, int chromeHeight = 40, int chromeWidth = 0);
// The windowed default: about one Toaster pixel per logical screen pixel, i.e. the device pixel
// ratio rounded up to a whole number, but never more than `fit` (and at least 1). The original was
// a full-screen program beside separate Program/Preview monitors; here it is one window among the
// monitor windows, so it opens at its natural size rather than filling the display.
int windowedZoom(int fit, double devicePixelRatio);
// fitZoom for the screen's available geometry (QScreen::availableGeometry), in its device pixels;
// chromeHeight and chromeWidth are in logical pixels.
int screenFitZoom(const QScreen&, int chromeHeight = 40, int chromeWidth = 0);
// windowedZoom(screenFitZoom(...), the screen's device pixel ratio): the zoom the app opens at.
int defaultZoom(const QScreen&, int chromeHeight = 40, int chromeWidth = 0);

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
// listing the drawers that hold croutons; choosing one fills the screen's Project grid. In order:
// those under <data42>/Effects ("4000FX/Wipes"), then <data42>/Framestore and its subfolders
// ("Framestore", "Framestore/CGDemo"), <data42>/CG/Frames ("CG/Frames"), the ToasterCG key, scroll and
// crawl pages ("CG/Keys", "CG/Scrolls", "CG/Crawls") and the folder SAVE writes to ("Saved framestores": listed even when it
// is empty or missing). The first drawer is shown at the start.
class ProjectView : public QWidget {
    Q_OBJECT
public:
    ProjectView(const std::filesystem::path& data42, const std::filesystem::path& savedFolder, ToasterScreen* screen,
                QWidget* parent = nullptr);
    QComboBox* drawerPicker() const { return picker_; }
    ToasterScreen* screen() const { return screen_; }
    void setSavedFolder(const std::filesystem::path&);  // re-reads it if it is the drawer shown
    void rescan();                                      // re-reads the drawer shown, keeping the scroll row

private:
    void showDrawer(int index);

    QComboBox* picker_;
    ToasterScreen* screen_;
    int savedAt_ = -1;  // the "Saved framestores" item
};

// The folders under `root` (any depth; `root` itself too if includeRoot) holding at least one
// effect file with a sibling icon, sorted by path (ASCII case-insensitive).
std::vector<std::filesystem::path> croutonDrawers(const std::filesystem::path& root, bool includeRoot = false);
