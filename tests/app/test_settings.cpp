#include "settings.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QLineEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QCheckBox>
#include <QTemporaryDir>
#include <QtTest>
#include <memory>

namespace {

using vt::media::InputConfig;
using vt::media::InputKind;

bool sameInput(const InputConfig& a, const InputConfig& b) {
    return a.kind == b.kind && a.path == b.path && a.loop == b.loop && a.videoDevice == b.videoDevice &&
           a.audioDevice == b.audioDevice && a.window == b.window && a.options == b.options && a.retryMs == b.retryMs;
}

// Every key a settings file holds, compared; names the first that differs.
QString firstDifference(const AppSettings& a, const AppSettings& b) {
    for (size_t i = 0; i < 4; ++i)
        if (!sameInput(a.inputs[i], b.inputs[i])) return QStringLiteral("input%1").arg(i + 1);
    if (a.toasterRoot != b.toasterRoot) return QStringLiteral("toasterRoot");
    if (a.recordingPath != b.recordingPath) return QStringLiteral("recording/path");
    if (a.recordingFormat != b.recordingFormat) return QStringLiteral("recording/format");
    if (a.audioOutDevice != b.audioOutDevice) return QStringLiteral("audioOutDevice");
    if (a.panelPalette != b.panelPalette) return QStringLiteral("panelPalette");
    if (a.screenZoom != b.screenZoom) return QStringLiteral("screenZoom");
    if (a.monitorZoom != b.monitorZoom) return QStringLiteral("monitorZoom");
    if (a.underscan != b.underscan) return QStringLiteral("underscan");
    if (a.windowGeometry != b.windowGeometry) return QStringLiteral("windowGeometry");
    return {};
}

// Settings with a non-default value in every key.
AppSettings filled(const QString& file) {
    AppSettings s(file);
    s.toasterRoot = QStringLiteral("D:/Toaster/data folder");
    InputConfig& file1 = s.inputs[0];
    file1.kind = InputKind::File;
    file1.path = "C:\\Videos\\clip one.mkv";
    file1.loop = false;
    file1.options = {{"video_size", "640x480"}, {"framerate", "30"}};
    file1.retryMs = 500;
    InputConfig& dev = s.inputs[1];
    dev.kind = InputKind::Device;
    dev.videoDevice = "@device_pnp_\\\\?\\usb#vid_046d&pid_0825#6&1a2b3c#{65e8773d-8f56-11d0-a3b9-00a0c9223196}\\global";
    dev.audioDevice = "@device_cm_{33D9A762-90C8-11D0-BD43-00A0C911CE86}\\wave_{12345678-0000-0000-0000-000000000000}";
    InputConfig& scr = s.inputs[2];
    scr.kind = InputKind::Screen;
    scr.window = "Notes, draft; v2 = final";  // separators that an INI file treats specially
    scr.audioDevice = "mic";
    s.inputs[3].kind = InputKind::None;
    s.inputs[3].path = "kept";
    s.recordingPath = QStringLiteral("E:/Recordings");
    s.recordingFormat = vt::media::RecordFormat::ProResMov;
    s.audioOutDevice = QStringLiteral("{0.0.0.00000000}.{abc}");
    s.panelPalette = PanelPalette::Preview;
    s.screenZoom = 3;
    s.monitorZoom = 4;
    s.underscan = true;
    s.windowGeometry = QByteArray("\x01\xd9\xd0\xcb\x00\x03", 6);
    return s;
}

DeviceLists lists() {
    DeviceLists d;
    d.video = {{"Cam A", "id-cam-a"}, {"Cam B", "id-cam-b"}};
    d.audio = {{"Mic A", "id-mic-a"}};
    d.output = {{"Speakers", "id-spk", true}, {"HDMI", "id-hdmi", false}};
    return d;
}

// The widget `name` of type T on the page of input n (1-4), or on the dialog's other pages if n is 0.
template <class T>
T* find(QWidget& dlg, int n, const char* name) {
    QWidget* root = n ? dlg.findChild<QWidget*>(QStringLiteral("input%1").arg(n)) : &dlg;
    return root->findChild<T*>(QString::fromLatin1(name));
}

}  // namespace

class TestSettings : public QObject {
    Q_OBJECT
private slots:
    void roundTripOfEveryKey();
    void defaultsWhenThereIsNoFile();
    void corruptValuesFallBackToDefaults();
    void toasterRootValidity();
    void dialogKeepsWhatItDoesNotEdit();
    void dialogEdits();
    void dialogNeedsAValidRoot();
};

void TestSettings::roundTripOfEveryKey() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString file = dir.filePath(QStringLiteral("sub/vtoast.ini"));  // the folder is made
    const AppSettings want = filled(file);
    QVERIFY(want.save());
    AppSettings got(file);
    got.load();
    const QString bad = firstDifference(got, want);
    QVERIFY2(bad.isEmpty(), qPrintable(bad));

    // Saving again replaces the file: an option that was dropped is gone.
    AppSettings fewer = want;
    fewer.inputs[0].options.erase("framerate");
    QVERIFY(fewer.save());
    got.load();
    QVERIFY2(firstDifference(got, fewer).isEmpty(), qPrintable(firstDifference(got, fewer)));
    QCOMPARE(got.inputs[0].options.size(), size_t(1));
}

void TestSettings::defaultsWhenThereIsNoFile() {
    QTemporaryDir dir;
    AppSettings s(dir.filePath(QStringLiteral("none.ini")));
    s.screenZoom = 5;
    s.load();  // back to the defaults
    const AppSettings def(s.file());
    QVERIFY2(firstDifference(s, def).isEmpty(), qPrintable(firstDifference(s, def)));
    QCOMPARE(def.monitorZoom, 1);
    QVERIFY(!def.underscan);
    QCOMPARE(def.panelPalette, PanelPalette::Rgb);
    QCOMPARE(def.recordingFormat, vt::media::RecordFormat::H264Mkv);
    QCOMPARE(def.inputs[0].kind, InputKind::None);
    QVERIFY(def.inputs[0].loop);
    QCOMPARE(def.inputs[0].retryMs, 2000);
    QVERIFY(AppSettings::defaultFile().endsWith(QStringLiteral("/vtoast_reborn/vtoast.ini")));
}

void TestSettings::corruptValuesFallBackToDefaults() {
    QTemporaryDir dir;
    const QString file = dir.filePath(QStringLiteral("bad.ini"));
    {
        QFile f(file);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("screenZoom=abc\nmonitorZoom=9\nunderscan=maybe\npanelPalette=purple\nsomethingNew=1\n"
                "[recording]\nformat=wmv\n"
                "[input1]\nkind=satellite\nloop=perhaps\nretryMs=-5\npath=ok.mkv\n"
                "[input2]\nkind=file\nretryMs=1.5\n");
    }
    AppSettings s(file);
    s.load();
    const AppSettings def(file);
    QCOMPARE(s.screenZoom, def.screenZoom);
    QCOMPARE(s.monitorZoom, def.monitorZoom);
    QCOMPARE(s.underscan, def.underscan);
    QCOMPARE(s.panelPalette, def.panelPalette);
    QCOMPARE(s.recordingFormat, def.recordingFormat);
    QCOMPARE(s.inputs[0].kind, InputKind::None);
    QCOMPARE(s.inputs[0].loop, def.inputs[0].loop);
    QCOMPARE(s.inputs[0].retryMs, def.inputs[0].retryMs);
    QCOMPARE(s.inputs[0].path, std::string("ok.mkv"));  // the valid keys beside the bad ones still load
    QCOMPARE(s.inputs[1].kind, InputKind::File);
    QCOMPARE(s.inputs[1].retryMs, def.inputs[1].retryMs);
    QCOMPARE(s.toasterRoot, def.toasterRoot);

    // Corrupt values are valid again after a save.
    {
        QFile f(file);
        QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        f.write("screenZoom=2\nmonitorZoom=3\n");
    }
    s.load();
    QCOMPARE(s.screenZoom, 2);
    QCOMPARE(s.monitorZoom, 3);
}

void TestSettings::toasterRootValidity() {
    QVERIFY(AppSettings::validToasterRoot(QStringLiteral(VT_DATA_ROOT)));
    QTemporaryDir dir;
    QVERIFY(!AppSettings::validToasterRoot(dir.path()));
    QVERIFY(!AppSettings::validToasterRoot(QString()));
    QVERIFY(QDir(dir.path()).mkpath(QStringLiteral("4.2/Programs")));
    QVERIFY(!AppSettings::validToasterRoot(dir.path()));  // needs the file itself
    QFile f(dir.filePath(QStringLiteral("4.2/Programs/switcher.main")));
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.close();
    QVERIFY(AppSettings::validToasterRoot(dir.path()));
}

// The dialog hands back what it was given, apart from what the user edits.
void TestSettings::dialogKeepsWhatItDoesNotEdit() {
    QTemporaryDir dir;
    AppSettings s = filled(dir.filePath(QStringLiteral("a.ini")));
    s.toasterRoot = QStringLiteral(VT_DATA_ROOT);
    s.audioOutDevice = QStringLiteral("id-hdmi");
    s.inputs[1].videoDevice = "id-cam-b";
    s.inputs[1].audioDevice = "id-mic-a";
    SettingsDialog dlg(s, lists());
    const AppSettings got = dlg.settings();
    QVERIFY2(firstDifference(got, s).isEmpty(), qPrintable(firstDifference(got, s)));
    QCOMPARE(got.file(), s.file());
    QCOMPARE(got.inputs[0].options.size(), size_t(2));  // no page for options, kept

    // A device that is not there any more stays selected.
    s.inputs[1].videoDevice = "gone";
    SettingsDialog dlg2(s, lists());
    QCOMPARE(dlg2.settings().inputs[1].videoDevice, std::string("gone"));
}

void TestSettings::dialogEdits() {
    AppSettings s{QString()};
    SettingsDialog dlg(s, lists());
    // Input 1: a file, no loop.
    find<QComboBox>(dlg, 1, "kind")->setCurrentIndex(1);
    find<QLineEdit>(dlg, 1, "path")->setText(QStringLiteral("C:/clip.mkv"));
    find<QCheckBox>(dlg, 1, "loop")->setChecked(false);
    // Input 2: a device.
    find<QComboBox>(dlg, 2, "kind")->setCurrentIndex(2);
    auto* video = find<QComboBox>(dlg, 2, "videoDevice");
    video->setCurrentIndex(video->findData(QStringLiteral("id-cam-b")));
    find<QComboBox>(dlg, 2, "deviceAudio")->setCurrentIndex(1);
    // Input 3: a window of the screen.
    find<QComboBox>(dlg, 3, "kind")->setCurrentIndex(3);
    find<QRadioButton>(dlg, 3, "windowRadio")->setChecked(true);
    find<QLineEdit>(dlg, 3, "window")->setText(QStringLiteral(" Game "));
    // Input 4: the whole desktop with sound.
    find<QComboBox>(dlg, 4, "kind")->setCurrentIndex(3);
    find<QComboBox>(dlg, 4, "screenAudio")->setCurrentIndex(1);

    auto* out = find<QComboBox>(dlg, 0, "audioOut");
    find<QComboBox>(dlg, 0, "recordingFormat")->setCurrentIndex(2);
    find<QLineEdit>(dlg, 0, "recordingPath")->setText(QStringLiteral("F:/rec"));
    out->setCurrentIndex(out->findData(QStringLiteral("id-hdmi")));
    find<QComboBox>(dlg, 0, "panelPalette")->setCurrentIndex(1);

    const AppSettings got = dlg.settings();
    QCOMPARE(got.inputs[0].kind, InputKind::File);
    QCOMPARE(got.inputs[0].path, std::string("C:/clip.mkv"));
    QVERIFY(!got.inputs[0].loop);
    QCOMPARE(got.inputs[1].kind, InputKind::Device);
    QCOMPARE(got.inputs[1].videoDevice, std::string("id-cam-b"));
    QCOMPARE(got.inputs[1].audioDevice, std::string("id-mic-a"));
    QCOMPARE(got.inputs[2].kind, InputKind::Screen);
    QCOMPARE(got.inputs[2].window, std::string("Game"));
    QCOMPARE(got.inputs[3].kind, InputKind::Screen);
    QCOMPARE(got.inputs[3].window, std::string());
    QCOMPARE(got.inputs[3].audioDevice, std::string("id-mic-a"));
    QCOMPARE(got.recordingFormat, vt::media::RecordFormat::Ffv1Mkv);
    QCOMPARE(got.recordingPath, QStringLiteral("F:/rec"));
    QCOMPARE(got.audioOutDevice, QStringLiteral("id-hdmi"));
    QCOMPARE(got.panelPalette, PanelPalette::Preview);
}

void TestSettings::dialogNeedsAValidRoot() {
    AppSettings s{QString()};
    s.toasterRoot = QStringLiteral(VT_DATA_ROOT);
    SettingsDialog dlg(s, lists());
    QPushButton* ok = dlg.findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok);
    QVERIFY(ok->isEnabled());
    auto* root = dlg.findChild<QLineEdit*>(QStringLiteral("toasterRoot"));
    root->setText(QStringLiteral("C:/definitely/not/here"));
    QVERIFY(!ok->isEnabled());
    root->setText(QStringLiteral(VT_DATA_ROOT));
    QVERIFY(ok->isEnabled());
}

std::unique_ptr<QObject> makeSettingsTests() { return std::make_unique<TestSettings>(); }

#include "test_settings.moc"
