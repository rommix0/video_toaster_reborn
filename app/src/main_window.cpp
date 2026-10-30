#include "main_window.h"

#include <QCloseEvent>
#include <QDockWidget>
#include <QLabel>
#include <QMenuBar>
#include <QStatusBar>
#include <QStyle>
#include <QTabBar>
#include <QTabWidget>
#include <QVBoxLayout>

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    setWindowTitle(QStringLiteral("vtoast — Video Toaster"));

    tabs_ = new QTabWidget(this);
    auto* page = new QWidget(tabs_);
    switcherBox_ = new QVBoxLayout(page);
    switcherBox_->setContentsMargins(0, 0, 0, 0);
    // A placeholder until AppController puts the Switcher screen (or the data-root message) here.
    auto* placeholder = new QLabel(QStringLiteral("Switcher"), page);
    placeholder->setAlignment(Qt::AlignCenter);
    setSwitcherWidget(placeholder);
    tabs_->addTab(page, QStringLiteral("Switcher"));
    for (const char* name : {"Paint", "LightWave", "Flyer"})
        tabs_->setTabEnabled(tabs_->addTab(new QWidget(tabs_), QString::fromLatin1(name)), false);
    setCentralWidget(tabs_);

    QMenu* file = menuBar()->addMenu(tr("&File"));
    settings_ = file->addAction(tr("&Settings…"));
    record_ = file->addAction(tr("Start Recording"));
    record_->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_R));
    file->addSeparator();
    quit_ = file->addAction(tr("&Quit"));
    quit_->setShortcut(QKeySequence::Quit);
    connect(quit_, &QAction::triggered, this, &QWidget::close);
    zoomMenu_ = menuBar()->addMenu(tr("&View"))->addMenu(tr("&Zoom"));
    windowMenu_ = menuBar()->addMenu(tr("&Window"));
    QMenu* help = menuBar()->addMenu(tr("&Help"));
    log_ = help->addAction(tr("&Log"));
    statusBar();  // created now, so chromeHeight() counts it
}

void MainWindow::setSwitcherWidget(QWidget* w) {
    delete switcher_;
    switcher_ = w;
    switcherBox_->addWidget(w);
}

int MainWindow::chromeHeight() const {
    return style()->pixelMetric(QStyle::PM_TitleBarHeight) + menuBar()->sizeHint().height() +
           tabs_->tabBar()->sizeHint().height() + 2 * style()->pixelMetric(QStyle::PM_DefaultFrameWidth) +
           statusBar()->sizeHint().height();
}

int MainWindow::chromeWidth() const {
    // The docks of one side are stacked in one column: the widest counts.
    int left = 0, right = 0;
    for (QDockWidget* d : findChildren<QDockWidget*>(Qt::FindDirectChildrenOnly)) {
        if (d->isHidden() || d->isFloating()) continue;
        const int dw = d->sizeHint().width() + style()->pixelMetric(QStyle::PM_DockWidgetSeparatorExtent);
        if (dockWidgetArea(d) == Qt::LeftDockWidgetArea) left = std::max(left, dw);
        if (dockWidgetArea(d) == Qt::RightDockWidgetArea) right = std::max(right, dw);
    }
    return 2 * style()->pixelMetric(QStyle::PM_DefaultFrameWidth) + left + right;
}

void MainWindow::closeEvent(QCloseEvent* e) {
    emit closing();
    QMainWindow::closeEvent(e);
}
