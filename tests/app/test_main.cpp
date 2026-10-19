// One Qt Test executable for the app: runs every test class in turn.
#include <QApplication>

int runMainWindowTests(int argc, char** argv);
int runToasterScreenTests(int argc, char** argv);

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    int failed = 0;
    for (auto* run : {runMainWindowTests, runToasterScreenTests}) failed += run(argc, argv);
    return failed;
}
