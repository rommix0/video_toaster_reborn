#include "settings.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QRadioButton>
#include <QSettings>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QTabWidget>
#include <QVBoxLayout>
#include <optional>

using vt::media::InputConfig;
using vt::media::InputKind;
using vt::media::RecordFormat;

namespace {

constexpr const char* kKindNames[] = {"none", "file", "device", "screen"};
constexpr const char* kFormatNames[] = {"h264-mkv", "h264-mp4", "ffv1-mkv", "prores-mov"};
constexpr const char* kPaletteNames[] = {"rgb", "preview"};

// The index in `names` of a stored word, or nullopt when it is none of them.
template <size_t N>
std::optional<int> indexOf(const char* const (&names)[N], const QString& word) {
    for (size_t i = 0; i < N; ++i)
        if (word == QLatin1String(names[i])) return int(i);
    return std::nullopt;
}

QString defaultRoot() {
#ifdef VT_DATA_ROOT
    return QDir::cleanPath(QStringLiteral(VT_DATA_ROOT));
#else
    return {};
#endif
}

// The stored text of a key. A text with commas that was not quoted reads back as a list: joined again.
QString text(const QSettings& s, const QString& key, const QString& def) {
    if (!s.contains(key)) return def;
    const QVariant v = s.value(key);
    return v.metaType().id() == QMetaType::QStringList ? v.toStringList().join(QLatin1Char(',')) : v.toString();
}

std::optional<int> integer(const QSettings& s, const QString& key, int lo, int hi) {
    bool ok = false;
    const int v = text(s, key, {}).toInt(&ok);
    return ok && v >= lo && v <= hi ? std::optional<int>(v) : std::nullopt;
}

std::optional<bool> boolean(const QSettings& s, const QString& key) {
    const QString v = text(s, key, {}).toLower();
    if (v == QLatin1String("true") || v == QLatin1String("1")) return true;
    if (v == QLatin1String("false") || v == QLatin1String("0")) return false;
    return std::nullopt;
}

std::string stdText(const QSettings& s, const QString& key, const std::string& def) {
    return s.contains(key) ? text(s, key, {}).toStdString() : def;
}

void readInput(QSettings& s, InputConfig& c) {
    if (const auto k = indexOf(kKindNames, text(s, QStringLiteral("kind"), {}))) c.kind = InputKind(*k);
    c.path = stdText(s, QStringLiteral("path"), c.path);
    if (const auto b = boolean(s, QStringLiteral("loop"))) c.loop = *b;
    c.videoDevice = stdText(s, QStringLiteral("videoDevice"), c.videoDevice);
    c.audioDevice = stdText(s, QStringLiteral("audioDevice"), c.audioDevice);
    c.window = stdText(s, QStringLiteral("window"), c.window);
    if (const auto r = integer(s, QStringLiteral("retryMs"), 100, 60000)) c.retryMs = *r;
    s.beginGroup(QStringLiteral("options"));
    c.options.clear();
    for (const QString& key : s.childKeys()) c.options[key.toStdString()] = text(s, key, {}).toStdString();
    s.endGroup();
}

void writeInput(QSettings& s, const InputConfig& c) {
    s.setValue(QStringLiteral("kind"), QLatin1String(kKindNames[int(c.kind)]));
    s.setValue(QStringLiteral("path"), QString::fromStdString(c.path));
    s.setValue(QStringLiteral("loop"), c.loop);
    s.setValue(QStringLiteral("videoDevice"), QString::fromStdString(c.videoDevice));
    s.setValue(QStringLiteral("audioDevice"), QString::fromStdString(c.audioDevice));
    s.setValue(QStringLiteral("window"), QString::fromStdString(c.window));
    s.setValue(QStringLiteral("retryMs"), c.retryMs);
    s.beginGroup(QStringLiteral("options"));
    for (const auto& [key, value] : c.options) s.setValue(QString::fromStdString(key), QString::fromStdString(value));
    s.endGroup();
}

}  // namespace

AppSettings::AppSettings(QString file)
    : toasterRoot(defaultRoot()),
      recordingPath(QStandardPaths::writableLocation(QStandardPaths::MoviesLocation)),
      file_(std::move(file)) {}

QString AppSettings::defaultFile() {
    QString appData = qEnvironmentVariable("APPDATA");
    if (appData.isEmpty()) appData = QDir::homePath() + QStringLiteral("/AppData/Roaming");
    return QDir::cleanPath(appData) + QStringLiteral("/vtoast_reborn/vtoast.ini");
}

bool AppSettings::validToasterRoot(const QString& root) {
    return !root.isEmpty() && QFileInfo(QDir(root).filePath(QStringLiteral("4.2/Programs/switcher.main"))).isFile();
}

void AppSettings::load() {
    AppSettings out(file_);  // the defaults
    QSettings s(file_, QSettings::IniFormat);

    if (const QString root = text(s, QStringLiteral("toasterRoot"), {}); !root.isEmpty()) out.toasterRoot = root;
    for (int i = 0; i < 4; ++i) {
        s.beginGroup(QStringLiteral("input%1").arg(i + 1));
        readInput(s, out.inputs[size_t(i)]);
        s.endGroup();
    }
    s.beginGroup(QStringLiteral("recording"));
    if (s.contains(QStringLiteral("path"))) out.recordingPath = text(s, QStringLiteral("path"), {});
    if (const auto f = indexOf(kFormatNames, text(s, QStringLiteral("format"), {}))) out.recordingFormat = RecordFormat(*f);
    s.endGroup();
    out.audioOutDevice = text(s, QStringLiteral("audioOutDevice"), out.audioOutDevice);
    if (const auto p = indexOf(kPaletteNames, text(s, QStringLiteral("panelPalette"), {}))) out.panelPalette = PanelPalette(*p);
    if (const auto z = integer(s, QStringLiteral("screenZoom"), 0, 8)) out.screenZoom = *z;
    if (const auto z = integer(s, QStringLiteral("monitorZoom"), 1, 4)) out.monitorZoom = *z;
    if (const auto u = boolean(s, QStringLiteral("underscan"))) out.underscan = *u;
    if (const QVariant g = s.value(QStringLiteral("windowGeometry")); g.metaType().id() == QMetaType::QByteArray)
        out.windowGeometry = g.toByteArray();
    *this = out;
}

bool AppSettings::save() const {
    QDir().mkpath(QFileInfo(file_).absolutePath());
    QSettings s(file_, QSettings::IniFormat);
    s.clear();
    s.setValue(QStringLiteral("toasterRoot"), toasterRoot);
    for (int i = 0; i < 4; ++i) {
        s.beginGroup(QStringLiteral("input%1").arg(i + 1));
        writeInput(s, inputs[size_t(i)]);
        s.endGroup();
    }
    s.beginGroup(QStringLiteral("recording"));
    s.setValue(QStringLiteral("path"), recordingPath);
    s.setValue(QStringLiteral("format"), QLatin1String(kFormatNames[int(recordingFormat)]));
    s.endGroup();
    s.setValue(QStringLiteral("audioOutDevice"), audioOutDevice);
    s.setValue(QStringLiteral("panelPalette"), QLatin1String(kPaletteNames[int(panelPalette)]));
    s.setValue(QStringLiteral("screenZoom"), screenZoom);
    s.setValue(QStringLiteral("monitorZoom"), monitorZoom);
    s.setValue(QStringLiteral("underscan"), underscan);
    if (!windowGeometry.isEmpty()) s.setValue(QStringLiteral("windowGeometry"), windowGeometry);
    s.sync();
    return s.status() == QSettings::NoError;
}

DeviceLists DeviceLists::live() {
    return {vt::media::listDevices(vt::media::DeviceKind::Video), vt::media::listDevices(vt::media::DeviceKind::Audio),
            vt::media::listOutputDevices()};
}

namespace {

struct Choice { QString name, id; };

// A combo of devices: a first item for "none" (id ""), then the devices. The current id is
// selected; one that is not in the list (a device that is unplugged now) is kept as an item of its own.
QComboBox* deviceCombo(const std::vector<Choice>& devices, const QString& noneText, const QString& current, QWidget* parent) {
    auto* box = new QComboBox(parent);
    box->addItem(noneText, QString());
    for (const auto& d : devices) {
        box->addItem(d.name, d.id);
        box->setItemData(box->count() - 1, d.id, Qt::ToolTipRole);
    }
    int at = box->findData(current);
    if (at < 0) {
        box->addItem(QObject::tr("%1 (not found)").arg(current), current);
        at = box->count() - 1;
    }
    box->setCurrentIndex(at);
    return box;
}

std::vector<Choice> choices(const std::vector<vt::media::DeviceInfo>& list) {
    std::vector<Choice> out;
    for (const auto& d : list) out.push_back({QString::fromStdString(d.name), QString::fromStdString(d.id)});
    return out;
}

QWidget* folderRow(QLineEdit* edit, QWidget* parent, const QString& caption) {
    auto* row = new QWidget(parent);
    auto* lay = new QHBoxLayout(row);
    lay->setContentsMargins(0, 0, 0, 0);
    auto* browse = new QPushButton(QObject::tr("Browse..."), row);
    lay->addWidget(edit, 1);
    lay->addWidget(browse);
    QObject::connect(browse, &QPushButton::clicked, edit, [edit, caption] {
        const QString dir = QFileDialog::getExistingDirectory(edit, caption, edit->text());
        if (!dir.isEmpty()) edit->setText(dir);
    });
    return row;
}

}  // namespace

// One input's page: its kind, and the fields of that kind. The InputConfig fields the page has no
// widget for (options, retryMs) and those of the other kinds stay as they were.
class SettingsDialog::InputPage : public QWidget {
public:
    InputPage(const InputConfig& config, const DeviceLists& devices, QWidget* parent) : QWidget(parent), base_(config) {
        auto* lay = new QVBoxLayout(this);
        kind_ = new QComboBox(this);
        kind_->setObjectName(QStringLiteral("kind"));
        for (const char* n : {"None", "File", "Device", "Screen"}) kind_->addItem(tr(n));
        kind_->setCurrentIndex(int(config.kind));
        auto* top = new QFormLayout;
        top->addRow(tr("Source:"), kind_);
        lay->addLayout(top);

        auto* stack = new QStackedWidget(this);
        lay->addWidget(stack);
        lay->addStretch(1);

        stack->addWidget(new QWidget(stack));  // None

        auto* file = new QWidget(stack);
        auto* fileForm = new QFormLayout(file);
        path_ = new QLineEdit(QString::fromStdString(config.path), file);
        path_->setObjectName(QStringLiteral("path"));
        auto* browse = new QPushButton(tr("Browse..."), file);
        auto* pathRow = new QHBoxLayout;
        pathRow->addWidget(path_, 1);
        pathRow->addWidget(browse);
        connect(browse, &QPushButton::clicked, this, [this] {
            const QString f = QFileDialog::getOpenFileName(this, tr("Video file"), path_->text());
            if (!f.isEmpty()) path_->setText(f);
        });
        loop_ = new QCheckBox(tr("Loop"), file);
        loop_->setObjectName(QStringLiteral("loop"));
        loop_->setChecked(config.loop);
        fileForm->addRow(tr("File:"), pathRow);
        fileForm->addRow(QString(), loop_);
        stack->addWidget(file);

        auto* device = new QWidget(stack);
        auto* deviceForm = new QFormLayout(device);
        video_ = deviceCombo(choices(devices.video), tr("(none)"), QString::fromStdString(config.videoDevice), device);
        video_->setObjectName(QStringLiteral("videoDevice"));
        deviceAudio_ = deviceCombo(choices(devices.audio), tr("(none)"), QString::fromStdString(config.audioDevice), device);
        deviceAudio_->setObjectName(QStringLiteral("deviceAudio"));
        deviceForm->addRow(tr("Video device:"), video_);
        deviceForm->addRow(tr("Audio device:"), deviceAudio_);
        stack->addWidget(device);

        auto* screen = new QWidget(stack);
        auto* screenForm = new QFormLayout(screen);
        desktop_ = new QRadioButton(tr("Whole desktop"), screen);
        desktop_->setObjectName(QStringLiteral("desktop"));
        auto* window = new QRadioButton(tr("Window with the title:"), screen);
        window->setObjectName(QStringLiteral("windowRadio"));
        window_ = new QLineEdit(QString::fromStdString(config.window), screen);
        window_->setObjectName(QStringLiteral("window"));
        (config.window.empty() ? desktop_ : window)->setChecked(true);
        window_->setEnabled(window->isChecked());
        connect(window, &QRadioButton::toggled, window_, &QLineEdit::setEnabled);
        screenAudio_ = deviceCombo(choices(devices.audio), tr("(none)"), QString::fromStdString(config.audioDevice), screen);
        screenAudio_->setObjectName(QStringLiteral("screenAudio"));
        screenForm->addRow(desktop_);
        screenForm->addRow(window, window_);
        screenForm->addRow(tr("Audio device:"), screenAudio_);
        stack->addWidget(screen);

        connect(kind_, &QComboBox::currentIndexChanged, stack, &QStackedWidget::setCurrentIndex);
        stack->setCurrentIndex(kind_->currentIndex());
    }

    InputConfig config() const {
        InputConfig c = base_;
        c.kind = InputKind(kind_->currentIndex());
        if (c.kind == InputKind::File) {
            c.path = path_->text().toStdString();
            c.loop = loop_->isChecked();
        } else if (c.kind == InputKind::Device) {
            c.videoDevice = video_->currentData().toString().toStdString();
            c.audioDevice = deviceAudio_->currentData().toString().toStdString();
        } else if (c.kind == InputKind::Screen) {
            c.window = desktop_->isChecked() ? std::string() : window_->text().trimmed().toStdString();
            c.audioDevice = screenAudio_->currentData().toString().toStdString();
        }
        return c;
    }

private:
    InputConfig base_;
    QComboBox *kind_, *video_, *deviceAudio_, *screenAudio_;
    QLineEdit *path_, *window_;
    QCheckBox* loop_;
    QRadioButton* desktop_;
};

SettingsDialog::SettingsDialog(const AppSettings& current, DeviceLists devices, QWidget* parent)
    : QDialog(parent), base_(current) {
    setWindowTitle(tr("Settings"));
    pages_ = new QListWidget(this);
    pages_->setFixedWidth(130);
    auto* stack = new QStackedWidget(this);
    auto add = [&](const QString& name, QWidget* page) {
        pages_->addItem(name);
        stack->addWidget(page);
    };
    add(tr("Inputs"), makeInputsPage(devices));
    add(tr("Recording"), makeRecordingPage());
    add(tr("Audio"), makeAudioPage(devices));
    add(tr("Toaster data"), makeToasterPage());
    add(tr("Display"), makeDisplayPage());
    connect(pages_, &QListWidget::currentRowChanged, stack, &QStackedWidget::setCurrentIndex);
    pages_->setCurrentRow(InputsPage);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    ok_ = buttons->button(QDialogButtonBox::Ok);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto* body = new QHBoxLayout;
    body->addWidget(pages_);
    body->addWidget(stack, 1);
    auto* lay = new QVBoxLayout(this);
    lay->addLayout(body, 1);
    lay->addWidget(buttons);
    checkRoot();
}

QWidget* SettingsDialog::makeInputsPage(const DeviceLists& devices) {
    auto* tabs = new QTabWidget(this);
    for (size_t i = 0; i < 4; ++i) {
        inputPages_[i] = new InputPage(base_.inputs[i], devices, tabs);
        inputPages_[i]->setObjectName(QStringLiteral("input%1").arg(i + 1));
        tabs->addTab(inputPages_[i], tr("Input %1").arg(i + 1));
    }
    return tabs;
}

QWidget* SettingsDialog::makeRecordingPage() {
    auto* page = new QWidget(this);
    auto* form = new QFormLayout(page);
    format_ = new QComboBox(page);
    format_->setObjectName(QStringLiteral("recordingFormat"));
    for (const char* n : {"H.264 in Matroska (.mkv)", "H.264 in MP4 (.mp4)", "FFV1 lossless in Matroska (.mkv)",
                          "ProRes in QuickTime (.mov)"})
        format_->addItem(tr(n));
    format_->setCurrentIndex(int(base_.recordingFormat));
    recordingPath_ = new QLineEdit(base_.recordingPath, page);
    recordingPath_->setObjectName(QStringLiteral("recordingPath"));
    form->addRow(tr("Format:"), format_);
    form->addRow(tr("Output folder:"), folderRow(recordingPath_, page, tr("Recording folder")));
    return page;
}

QWidget* SettingsDialog::makeAudioPage(const DeviceLists& devices) {
    auto* page = new QWidget(this);
    auto* form = new QFormLayout(page);
    std::vector<Choice> out;
    for (const auto& d : devices.output)
        out.push_back({QString::fromStdString(d.name) + (d.isDefault ? tr(" (default)") : QString()), QString::fromStdString(d.id)});
    audioOut_ = deviceCombo(out, tr("Default device"), base_.audioOutDevice, page);
    audioOut_->setObjectName(QStringLiteral("audioOut"));
    form->addRow(tr("Output device:"), audioOut_);
    return page;
}

QWidget* SettingsDialog::makeToasterPage() {
    auto* page = new QWidget(this);
    auto* form = new QFormLayout(page);
    root_ = new QLineEdit(base_.toasterRoot, page);
    root_->setObjectName(QStringLiteral("toasterRoot"));
    rootStatus_ = new QLabel(page);
    form->addRow(tr("Data folder:"), folderRow(root_, page, tr("Toaster data folder")));
    form->addRow(QString(), rootStatus_);
    connect(root_, &QLineEdit::textChanged, this, &SettingsDialog::checkRoot);
    return page;
}

QWidget* SettingsDialog::makeDisplayPage() {
    auto* page = new QWidget(this);
    auto* form = new QFormLayout(page);
    palette_ = new QComboBox(page);
    palette_->setObjectName(QStringLiteral("panelPalette"));
    palette_->addItem(tr("RGB mode (000 555 999 DDD)"));
    palette_->addItem(tr("Toaster-encoded, look unverified (000 842 7BC FFF)"));
    palette_->setCurrentIndex(int(base_.panelPalette));
    form->addRow(tr("Panel palette:"), palette_);
    return page;
}

void SettingsDialog::checkRoot() {
    const bool valid = AppSettings::validToasterRoot(root_->text());
    rootStatus_->setText(valid ? tr("Found 4.2/Programs/switcher.main.")
                               : tr("Not a Toaster data folder: 4.2/Programs/switcher.main is missing."));
    ok_->setEnabled(valid);
}

void SettingsDialog::showPage(Page p) { pages_->setCurrentRow(p); }

AppSettings SettingsDialog::settings() const {
    AppSettings s = base_;
    for (size_t i = 0; i < 4; ++i) s.inputs[i] = inputPages_[i]->config();
    s.recordingFormat = RecordFormat(format_->currentIndex());
    s.recordingPath = recordingPath_->text();
    s.audioOutDevice = audioOut_->currentData().toString();
    s.toasterRoot = root_->text();
    s.panelPalette = PanelPalette(palette_->currentIndex());
    return s;
}
