// One Qt Test executable for the app's test classes, set up as QTEST_MAIN does. Options go to every
// class; a test function name goes only to the class that has it, and only classes named that way run
// (so `vt_app_tests paletteAndExactZoom` runs just that). When several classes run, each "-o file"
// gets "-<ClassName>" before its extension, so no class overwrites another's results.
#include <QApplication>
#include <QFileInfo>
#include <QMetaMethod>
#include <QtTest>
#include <algorithm>
#include <memory>
#include <vector>

std::unique_ptr<QObject> makeMainWindowTests();
std::unique_ptr<QObject> makeToasterScreenTests();
std::unique_ptr<QObject> makeProjectGridTests();
std::unique_ptr<QObject> makeMonitorTests();
std::unique_ptr<QObject> makeMixerDockTests();
std::unique_ptr<QObject> makeSettingsTests();

namespace {

bool takesValue(const QByteArray& option) {
    static const QByteArrayList withValue{"-o", "-eventdelay", "-keydelay", "-mousedelay", "-maxwarnings",
                                          "-repeat", "-minimumvalue", "-minimumtotal", "-iterations", "-median"};
    return withValue.contains(option);
}

// QtTest runs a class's private parameterless slots; a selector is "function[:datatag]".
bool hasTestFunction(const QObject& test, const QByteArray& selector) {
    const QByteArray name = selector.split(':').first();
    const QMetaObject* mo = test.metaObject();
    for (int i = mo->methodOffset(); i < mo->methodCount(); ++i) {
        const QMetaMethod m = mo->method(i);
        if (m.methodType() == QMetaMethod::Slot && m.access() == QMetaMethod::Private && m.parameterCount() == 0 &&
            m.name() == name)
            return true;
    }
    return false;
}

// "-o out.txt,txt" -> "out-TestX.txt,txt"; "-" (stdout) stays.
QByteArray perClassOutput(const QByteArray& value, const char* className) {
    const qsizetype comma = value.indexOf(',');
    const QByteArray file = comma < 0 ? value : value.left(comma), rest = comma < 0 ? QByteArray() : value.mid(comma);
    if (file == "-") return value;
    const QFileInfo fi(QString::fromLocal8Bit(file));
    const QString suffix = fi.suffix().isEmpty() ? QString() : QStringLiteral(".") + fi.suffix();
    const QString renamed = fi.path() + QStringLiteral("/") + fi.completeBaseName() + QStringLiteral("-") +
                            QString::fromLatin1(className) + suffix;
    return renamed.toLocal8Bit() + rest;
}

}  // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);  // takes Qt's own arguments (-platform ...) out of argv
    app.setAttribute(Qt::AA_Use96Dpi, true);
    QTEST_DISABLE_KEYPAD_NAVIGATION
    QTEST_SET_MAIN_SOURCE_PATH

    std::vector<std::unique_ptr<QObject>> tests;
    tests.push_back(makeMainWindowTests());
    tests.push_back(makeToasterScreenTests());
    tests.push_back(makeProjectGridTests());
    tests.push_back(makeMonitorTests());
    tests.push_back(makeMixerDockTests());
    tests.push_back(makeSettingsTests());

    QByteArrayList options, selectors;
    for (int i = 1; i < argc; ++i) {
        const QByteArray a = argv[i];
        if (!a.startsWith('-')) {
            selectors << a;
            continue;
        }
        options << a;
        if (takesValue(a) && i + 1 < argc) options << argv[++i];
    }

    // Which classes run, each with its own selectors; an unknown name goes to the first class,
    // so QtTest reports it.
    std::vector<std::pair<QObject*, QByteArrayList>> runs;
    auto runOf = [&](QObject* t) -> QByteArrayList& {
        for (auto& r : runs)
            if (r.first == t) return r.second;
        return runs.emplace_back(t, QByteArrayList{}).second;
    };
    if (selectors.isEmpty())
        for (auto& t : tests) runOf(t.get());
    for (const auto& s : selectors) {
        const auto owner = std::find_if(tests.begin(), tests.end(), [&](const auto& t) { return hasTestFunction(*t, s); });
        runOf(owner != tests.end() ? owner->get() : tests.front().get()) << s;
    }

    int failed = 0;
    for (auto& [test, own] : runs) {
        QByteArrayList args{argv[0]};
        for (qsizetype i = 0; i < options.size(); ++i) {
            args << options[i];
            if (options[i] == "-o" && i + 1 < options.size() && runs.size() > 1)
                args << perClassOutput(options[++i], test->metaObject()->className());
        }
        args << own;
        std::vector<char*> ptrs;
        for (auto& a : args) ptrs.push_back(a.data());
        failed += QTest::qExec(test, int(ptrs.size()), ptrs.data());
    }
    return std::min(failed, 127);
}
