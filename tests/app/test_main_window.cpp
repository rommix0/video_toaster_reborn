#include "main_window.h"

#include <QTabWidget>
#include <QtTest>
#include <memory>

class TestMainWindow : public QObject {
    Q_OBJECT
private slots:
    void tabs();
    void title();
};

void TestMainWindow::tabs() {
    MainWindow w;
    auto* tabs = w.findChild<QTabWidget*>();
    QVERIFY(tabs);
    QCOMPARE(tabs->count(), 3);
    QStringList names; for (int i = 0; i < 3; ++i) names << tabs->tabText(i);
    QCOMPARE(names, (QStringList{"Switcher", "Paint", "Flyer"}));
    QVERIFY(tabs->isTabEnabled(0));
    for (int i = 1; i < 3; ++i) QVERIFY(!tabs->isTabEnabled(i));
}

void TestMainWindow::title() {
    MainWindow w;
    QCOMPARE(w.windowTitle(), QStringLiteral("vtoast — Video Toaster"));
}

std::unique_ptr<QObject> makeMainWindowTests() { return std::make_unique<TestMainWindow>(); }

#include "test_main_window.moc"
