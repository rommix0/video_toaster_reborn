#pragma once
#include <QMainWindow>

class QAction;
class QMenu;
class QTabWidget;
class QVBoxLayout;

// The vtoast main window: one tab per Video Toaster module (only the Switcher is live), the File
// menu (Settings…, Start/Stop Recording, Quit), View -> Zoom (the Switcher screen's zoom), the
// Window menu (the monitors and the mixer), Help -> Log, and the status bar. AppController fills the Switcher tab and acts on the menus.
class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);

    // The Switcher tab's content; the previous one is deleted.
    void setSwitcherWidget(QWidget*);
    QWidget* switcherWidget() const { return switcher_; }

    QAction* settingsAction() const { return settings_; }
    QAction* recordAction() const { return record_; }  // its text says Start, Stop or Stopping…
    QAction* quitAction() const { return quit_; }
    QAction* logAction() const { return log_; }
    QMenu* zoomMenu() const { return zoomMenu_; }      // AppController fills it
    QMenu* windowMenu() const { return windowMenu_; }

    // Logical pixels the window adds above and below the Switcher tab's content: the title bar,
    // menu bar, tab bar with its frame, and status bar.
    int chromeHeight() const;
    // Logical pixels the window adds beside the Switcher tab's content: the tab frame and the docks
    // (not hidden, not floating) on the left and right, with their separators.
    int chromeWidth() const;

signals:
    void closing();  // the window is about to close (sent from closeEvent)

protected:
    void closeEvent(QCloseEvent*) override;

private:
    QTabWidget* tabs_;
    QVBoxLayout* switcherBox_;
    QWidget* switcher_ = nullptr;
    QAction *settings_, *record_, *quit_, *log_;
    QMenu *zoomMenu_, *windowMenu_;
};
