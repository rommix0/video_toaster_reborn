#include "main_window.h"

#include <QTabWidget>
#include <QtTest>

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
    QCOMPARE(tabs->count(), 4);
    QStringList names; for (int i = 0; i < 4; ++i) names << tabs->tabText(i);
    QCOMPARE(names, (QStringList{"Switcher", "Paint", "LightWave", "Flyer"}));
    QVERIFY(tabs->isTabEnabled(0));
    for (int i = 1; i < 4; ++i) QVERIFY(!tabs->isTabEnabled(i));
}

void TestMainWindow::title() {
    MainWindow w;
    QCOMPARE(w.windowTitle(), QStringLiteral("vtoast — Video Toaster"));
}

int runMainWindowTests(int argc, char** argv) {
    TestMainWindow t;
    return QTest::qExec(&t, argc, argv);
}

#include "test_main_window.moc"
