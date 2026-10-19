#include "controller.h"

#include <QApplication>

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    // The monitors are windows of their own: the app ends when the main window closes and
    // AppController has shut the loop, the recording and the audio down.
    app.setQuitOnLastWindowClosed(false);
    AppSettings settings;
    settings.load();
    AppController controller(settings, AppController::Mode::Realtime);
    QObject::connect(&controller, &AppController::finished, &app, &QCoreApplication::quit, Qt::QueuedConnection);
    controller.show();
    return app.exec();
}
