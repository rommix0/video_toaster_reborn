#pragma once
#include <QMainWindow>

// The vtoast main window: one tab per Video Toaster module. Only the Switcher is live so far.
class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);
};
