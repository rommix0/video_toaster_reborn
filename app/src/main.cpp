#include "main_window.h"

#include <QApplication>

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    MainWindow window;
    window.resize(1280, 800);
    window.show();
    return app.exec();
}
