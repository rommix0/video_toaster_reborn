#include "main_window.h"

#include <QLabel>
#include <QTabWidget>

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    setWindowTitle(QStringLiteral("vtoast — Video Toaster"));

    auto* tabs = new QTabWidget(this);
    // The Switcher tab holds a placeholder until the switcher screen exists.
    auto* switcher = new QLabel(QStringLiteral("Switcher"), tabs);
    switcher->setAlignment(Qt::AlignCenter);
    tabs->addTab(switcher, QStringLiteral("Switcher"));
    for (const char* name : {"Paint", "LightWave", "Flyer"})
        tabs->setTabEnabled(tabs->addTab(new QWidget(tabs), QString::fromLatin1(name)), false);
    setCentralWidget(tabs);
}
