#include "controller.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QComboBox>
#include <QDateTime>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QLabel>
#include <QMenu>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QStatusBar>
#include <QVBoxLayout>
#include <algorithm>
#include <deque>
#include <exception>
#include <stdexcept>
#include <string>
#include <vector>

#include "main_window.h"
#include "mixer_dock.h"
#include "monitor_window.h"
#include "project_grid.h"
#include "toaster_screen.h"
#include "vt/assets/font.h"
#include "vt/assets/hunk.h"
#include "vt/assets/panel.h"
#include "vt/effect.h"
#include "vt/log.h"
#include "vt/media/audio_out.h"
#include "vt/media/program_loop.h"
#include "vt/media/recorder.h"

namespace fs = std::filesystem;
using vt::media::InputKind;
using vt::media::InputStatus;
using vt::media::RecordFormat;

namespace {

constexpr size_t kMaxQueuedLines = 1000;
constexpr int kMaxLogLines = 5000;

fs::path toPath(const QString& s) { return fs::path(s.toStdU16String()); }

// The whole file as an Amiga hunk image; throws std::runtime_error naming the file.
vt::assets::HunkImage hunks(const QString& file) {
    QFile f(file);
    if (!f.open(QIODevice::ReadOnly)) throw std::runtime_error("cannot read " + QDir::toNativeSeparators(file).toStdString());
    const QByteArray b = f.readAll();
    return vt::assets::loadHunks({reinterpret_cast<const uint8_t*>(b.constData()), size_t(b.size())});
}

const char* extension(RecordFormat f) {
    switch (f) {
    case RecordFormat::H264Mp4: return "mp4";
    case RecordFormat::ProResMov: return "mov";
    default: return "mkv";
    }
}

bool sameInput(const vt::media::InputConfig& a, const vt::media::InputConfig& b) {
    return a.kind == b.kind && a.path == b.path && a.loop == b.loop && a.videoDevice == b.videoDevice &&
           a.audioDevice == b.audioDevice && a.window == b.window && a.options == b.options && a.retryMs == b.retryMs;
}

QString stateWord(InputStatus::State s) {
    switch (s) {
    case InputStatus::State::Idle: return QStringLiteral("idle");
    case InputStatus::State::Opening: return QStringLiteral("opening");
    case InputStatus::State::Running: return QStringLiteral("on");
    case InputStatus::State::Ended: return QStringLiteral("ended");
    case InputStatus::State::Error: return QStringLiteral("error");
    }
    return {};
}

}  // namespace

// What the log sink leaves for the UI thread.
struct AppController::LogQueue {
    std::mutex mu;
    std::deque<std::string> lines;
    uint64_t dropped = 0;  // lines that did not fit
};

AppController::AppController(AppSettings& settings, Mode mode, QObject* parent)
    : QObject(parent), settings_(settings), mode_(mode), log_(std::make_shared<LogQueue>()) {
    vt::setLogSink([q = log_](std::string_view line) {
        const std::lock_guard l(q->mu);
        if (q->lines.size() >= kMaxQueuedLines) {
            q->lines.pop_front();
            ++q->dropped;
        }
        q->lines.emplace_back(line);
    });

    vt::media::ProgramConfig config;
    config.inputs = settings_.inputs;
    loop_ = std::make_unique<vt::media::ProgramLoop>(std::move(config));
    recorder_ = std::make_unique<vt::media::Recorder>();
    loop_->addSink(recorder_.get());
    if (mode_ == Mode::Realtime) {
        audio_ = std::make_unique<vt::media::AudioOutput>();
        loop_->addSink(audio_.get());
    }

    program_ = std::make_unique<MonitorWindow>(MonitorWindow::Kind::Program);
    preview_ = std::make_unique<MonitorWindow>(MonitorWindow::Kind::Preview);
    for (MonitorWindow* m : {program_.get(), preview_.get()}) {
        m->setZoom(settings_.monitorZoom);
        m->setUnderscan(settings_.underscan);
        connect(m, &MonitorWindow::zoomChanged, this, [this](int z) { settings_.monitorZoom = z; });
        connect(m, &MonitorWindow::underscanChanged, this, [this](bool u) { settings_.underscan = u; });
        loop_->addMonitor(&m->sink());
    }

    window_ = std::make_unique<MainWindow>();
    mixer_ = new MixerDock(loop_->mixer(), window_.get());
    window_->addDockWidget(Qt::RightDockWidgetArea, mixer_);
    connect(window_.get(), &MainWindow::closing, this, &AppController::shutdown);
    connect(window_->settingsAction(), &QAction::triggered, this, [this] { openSettings(); });
    connect(window_->recordAction(), &QAction::triggered, this, [this] { recording_ ? stopRecording() : startRecording(); });
    QMenu* windows = window_->windowMenu();
    for (MonitorWindow* m : {program_.get(), preview_.get()})
        connect(windows->addAction(m->windowTitle()), &QAction::triggered, m, [m] {
            m->show();
            m->raise();
        });
    windows->addAction(mixer_->toggleViewAction());
    zoomGroup_ = new QActionGroup(this);
    connect(window_->zoomMenu(), &QMenu::aboutToShow, this, &AppController::fillZoomMenu);

    auto* logWindow = new QDialog(window_.get());
    logWindow->setWindowTitle(tr("Log"));
    logWindow->resize(720, 360);
    logView_ = new QPlainTextEdit(logWindow);
    logView_->setObjectName(QStringLiteral("logView"));
    logView_->setReadOnly(true);
    logView_->setMaximumBlockCount(kMaxLogLines);
    (new QVBoxLayout(logWindow))->addWidget(logView_);
    connect(window_->logAction(), &QAction::triggered, logWindow, [logWindow] {
        logWindow->show();
        logWindow->raise();
    });

    inputsLabel_ = new QLabel(window_.get());
    inputsLabel_->setObjectName(QStringLiteral("inputStatus"));
    recordLabel_ = new QLabel(window_.get());
    recordLabel_->setObjectName(QStringLiteral("recordStatus"));
    loopLabel_ = new QLabel(window_.get());
    loopLabel_->setObjectName(QStringLiteral("loopStatus"));
    for (QLabel* l : {inputsLabel_, recordLabel_, loopLabel_}) window_->statusBar()->addPermanentWidget(l);

    window_->resize(800, 500);  // until the panel sets its own size, or the stored geometry
    buildPanel();
    if (!settings_.windowGeometry.isEmpty()) window_->restoreGeometry(settings_.windowGeometry);
    fillZoomMenu();
    updateRecordAction();
    updateStatus();

    if (mode_ == Mode::Realtime) {
        loop_->start();
        startAudio();
    }
    connect(&timer_, &QTimer::timeout, this, &AppController::poll);
    timer_.start(1000 / 30);
}

AppController::~AppController() {
    shutdown();
    vt::setLogSink({});
}

ToasterScreen* AppController::screen() const { return screen_; }
ProjectView* AppController::projectView() const { return view_; }

void AppController::show() {
    window_->show();
    program_->show();
    preview_->show();
}

void AppController::runOffline(int64_t ticks) {
    loop_->runOffline(ticks);
    poll();
}

// ---- the Switcher screen ----

void AppController::buildPanel() {
    const QString root = settings_.toasterRoot;
    if (!AppSettings::validToasterRoot(root)) return showDataMessage({});
    try {
        const QDir dir(root);
        const auto layout = vt::assets::loadPanel(hunks(dir.filePath(QStringLiteral("4.2/Programs/switcher.main"))));
        const auto font = vt::assets::loadColorFont(
            hunks(dir.filePath(QStringLiteral("4.2/Programs/System_Support/Fonts/CGInterface/14.4C"))));
        auto* screen = new ToasterScreen(layout, font, *this);
        auto* view = new ProjectView(toPath(dir.filePath(QStringLiteral("4.2/Effects"))), screen);
        screen->setPanelPalette(settings_.panelPalette == PanelPalette::Preview);
        // The drawer list must not keep the keys (Space, Return, digits) from the screen.
        connect(view->drawerPicker(), &QComboBox::activated, screen, [screen] { screen->setFocus(); });
        window_->setSwitcherWidget(view);
        screen_ = screen;
        view_ = view;
        screen->setZoom(settings_.screenZoom > 0 ? settings_.screenZoom : fitScreenZoom());
        screen->setState(loop_->switcherState());
        screen->setFocus();
        if (settings_.windowGeometry.isEmpty()) window_->resize(window_->sizeHint());
    } catch (const std::exception& e) {
        showDataMessage(QString::fromStdString(e.what()));
    }
}

int AppController::fitScreenZoom() const {
    int chrome = window_->chromeHeight();
    if (view_) chrome += view_->drawerPicker()->sizeHint().height() + view_->layout()->spacing();
    return defaultZoom(*window_->screen(), chrome, window_->chromeWidth());
}

void AppController::setScreenZoom(int z) {
    settings_.screenZoom = std::clamp(z, 0, 8);
    applyScreenZoom();
}

void AppController::applyScreenZoom() {
    if (!screen_) return;
    screen_->setZoom(settings_.screenZoom > 0 ? settings_.screenZoom : fitScreenZoom());
    screen_->updateGeometry();
    if (!(window_->windowState() & (Qt::WindowMaximized | Qt::WindowFullScreen))) window_->resize(window_->sizeHint());
}

// Fit, then 1x up to the zoom that fits the display (or the current zoom, if larger); rebuilt each
// time the menu opens, so it follows the display the window is on.
void AppController::fillZoomMenu() {
    QMenu* menu = window_->zoomMenu();
    menu->clear();
    const int top = std::max(fitScreenZoom(), settings_.screenZoom);
    for (int z = 0; z <= top; ++z) {
        QAction* a = menu->addAction(z ? tr("%1×").arg(z) : tr("Fit"));
        a->setCheckable(true);
        a->setChecked(z == settings_.screenZoom);
        a->setEnabled(screen_ != nullptr);
        zoomGroup_->addAction(a);
        connect(a, &QAction::triggered, this, [this, z] { setScreenZoom(z); });
        if (!z) menu->addSeparator();
    }
}

void AppController::showDataMessage(const QString& why) {
    auto* page = new QWidget;
    page->setObjectName(QStringLiteral("dataRootMessage"));
    auto* box = new QVBoxLayout(page);
    QString text = tr("Toaster data not found at %1").arg(QDir::toNativeSeparators(settings_.toasterRoot));
    if (!why.isEmpty()) text += QStringLiteral("\n") + why;
    auto* label = new QLabel(text, page);
    label->setAlignment(Qt::AlignCenter);
    label->setWordWrap(true);
    auto* choose = new QPushButton(tr("Choose the Toaster data folder…"), page);
    connect(choose, &QPushButton::clicked, this, [this] { openSettings(SettingsDialog::ToasterPage); });
    box->addStretch(1);
    box->addWidget(label);
    box->addWidget(choose, 0, Qt::AlignCenter);
    box->addStretch(1);
    window_->setSwitcherWidget(page);
    screen_ = nullptr;
    view_ = nullptr;
}

// ---- actions ----

void AppController::select(vt::Bus b, vt::Source s) {
    loop_->post([b, s](vt::Switcher& sw) { sw.select(b, s); });
}

void AppController::take() {
    loop_->post([](vt::Switcher& sw) { sw.take(); });
}

void AppController::autoTransition(vt::Direction d) {
    loop_->post([d](vt::Switcher& sw) { sw.autoTransition(sw.speed(), d); });
}

void AppController::setSpeed(vt::Speed s) {
    loop_->post([s](vt::Switcher& sw) { sw.setSpeed(s); });
}

void AppController::setTBar(int position) {
    loop_->post([position](vt::Switcher& sw) { sw.setTBar(position); });
}

void AppController::setBackground(vt::BkgColour c) {
    loop_->post([c](vt::Switcher& sw) { sw.setBackground(c); });
}

void AppController::loadCrouton(int index, bool run) {
    if (!screen_) return;
    const auto& croutons = screen_->project().croutons();
    if (index < 0 || index >= int(croutons.size())) return;
    const vt::assets::Crouton& c = croutons[size_t(index)];
    std::shared_ptr<const vt::Effect> effect;
    if (const auto hit = std::find_if(effects_.begin(), effects_.end(), [&](const auto& e) { return e.first == c.effect; });
        hit != effects_.end()) {
        effect = hit->second;
        effects_.erase(hit);
    } else {
        std::string why;
        effect = vt::loadEffect(c.effect, &why);
        if (!effect) {
            message(tr("Cannot load %1: %2").arg(QString::fromStdString(c.name), QString::fromStdString(why)));
            return;
        }
    }
    effects_.emplace_front(c.effect, effect);
    if (effects_.size() > kEffectCache) effects_.pop_back();
    loop_->postChecked([this, e = effect, path = c.effect, run](vt::Switcher& sw, std::string& why) {
        if (!sw.setEffect(e, &why)) return false;
        {
            const std::lock_guard l(effectMu_);
            currentEffect_ = path;
        }
        if (run) sw.autoTransition(sw.speed());
        return true;
    });
}

void AppController::notImplemented(const QString& what) { message(tr("%1: not in version 1").arg(what)); }

// ---- the 30 Hz poll ----

void AppController::poll() {
    if (stopping_.valid() && stopping_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        stopping_.get();
        recording_ = false;
        const auto st = recorder_->status();
        message(st.error.empty() ? tr("Recorded %1").arg(QDir::toNativeSeparators(recordingFile_))
                                 : tr("Recording failed: %1").arg(QString::fromStdString(st.error)));
        updateRecordAction();
    }

    std::deque<std::string> lines;
    uint64_t dropped = 0;
    {
        const std::lock_guard l(log_->mu);
        lines.swap(log_->lines);
        std::swap(dropped, log_->dropped);
    }
    if (dropped) logView_->appendPlainText(tr("(%1 more lines were dropped)").arg(dropped));
    for (const auto& line : lines) logView_->appendPlainText(QString::fromStdString(line));
    if (!lines.empty()) message(QString::fromStdString(lines.back()), 5000);

    const vt::media::SwitcherSnapshot s = loop_->switcherState();
    if (s.refusalCount != refusalsShown_) {
        refusalsShown_ = s.refusalCount;
        if (!s.lastRefusal.empty()) message(tr("Refused: %1").arg(QString::fromStdString(s.lastRefusal)));
    }
    if (screen_) {
        screen_->setState(s);
        fs::path current;
        {
            const std::lock_guard l(effectMu_);
            current = currentEffect_;
        }
        int loaded = -1;
        const auto& croutons = screen_->project().croutons();
        for (size_t i = 0; i < croutons.size() && !current.empty(); ++i)
            if (croutons[i].effect == current) loaded = int(i);
        screen_->setLoaded(loaded);
    }
    updateStatus();
}

void AppController::message(const QString& text, int ms) { window_->statusBar()->showMessage(text, ms); }

void AppController::updateStatus() {
    QStringList parts, tips;
    for (int i = 0; i < 4; ++i) {
        const InputStatus st = loop_->inputStatus(i);
        const bool none = settings_.inputs[size_t(i)].kind == InputKind::None;
        parts << QStringLiteral("In%1 %2").arg(i + 1).arg(none ? QStringLiteral("-") : stateWord(st.state));
        if (!none) tips << QStringLiteral("In%1: %2").arg(i + 1).arg(QString::fromStdString(st.message));
    }
    inputsLabel_->setText(parts.join(QStringLiteral("  ")));
    inputsLabel_->setToolTip(tips.join(QLatin1Char('\n')));

    if (recording_) {
        const auto st = recorder_->status();
        QString text = stopping_.valid() ? tr("Stopping recording") : tr("REC");
        text += tr(" %1 frames").arg(st.framesWritten);
        if (st.framesDropped) text += tr(", %1 dropped").arg(st.framesDropped);
        if (!st.error.empty()) text = tr("Recording failed: %1").arg(QString::fromStdString(st.error));
        recordLabel_->setText(text);
    } else {
        recordLabel_->setText(tr("Not recording"));
    }

    const auto stats = loop_->stats();
    loopLabel_->setText(tr("Late ticks %1").arg(stats.lateTicks) +
                        (stats.stalls ? tr(", stalls %1").arg(stats.stalls) : QString()));
}

// ---- recording ----

void AppController::startRecording() {
    if (recording_ || shutDown_) return;
    QDir dir(settings_.recordingPath);
    if (!dir.mkpath(QStringLiteral("."))) {
        message(tr("Cannot create the recording folder %1").arg(QDir::toNativeSeparators(settings_.recordingPath)));
        return;
    }
    const QString name = QStringLiteral("vtoast-%1.%2")
                             .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss")),
                                  QLatin1String(extension(settings_.recordingFormat)));
    vt::media::RecorderSettings rs;
    rs.path = toPath(dir.filePath(name));
    rs.format = settings_.recordingFormat;
    rs.blockWhenFull = mode_ == Mode::Offline;
    std::string why;
    if (!recorder_->start(rs, &why)) {
        message(tr("Cannot record: %1").arg(QString::fromStdString(why)));
        return;
    }
    recordingFile_ = dir.filePath(name);
    recording_ = true;
    message(tr("Recording to %1").arg(QDir::toNativeSeparators(recordingFile_)));
    updateRecordAction();
    updateStatus();
}

void AppController::stopRecording() {
    if (!recording_ || stopping_.valid()) return;
    stopping_ = std::async(std::launch::async, [r = recorder_.get()] { r->stop(); });
    updateRecordAction();
    updateStatus();
}

void AppController::updateRecordAction() {
    QAction* a = window_->recordAction();
    a->setEnabled(!stopping_.valid() && !shutDown_);
    a->setText(stopping_.valid() ? tr("Stopping…") : recording_ ? tr("Stop Recording") : tr("Start Recording"));
}

// ---- settings ----

void AppController::openSettings(SettingsDialog::Page page) {
    if (settingsDialog_) {
        settingsDialog_->showPage(page);
        settingsDialog_->raise();
        return;
    }
    DeviceLists devices;
    if (mode_ == Mode::Realtime) {  // DirectShow enumeration blocks for a moment
        QApplication::setOverrideCursor(Qt::WaitCursor);
        devices = DeviceLists::live();
        QApplication::restoreOverrideCursor();
    }
    settingsDialog_ = new SettingsDialog(settings_, std::move(devices), window_.get());
    settingsDialog_->setAttribute(Qt::WA_DeleteOnClose);
    connect(settingsDialog_, &QDialog::accepted, this, [this, d = settingsDialog_.data()] { applySettings(d->settings()); });
    settingsDialog_->showPage(page);
    settingsDialog_->open();
}

void AppController::applySettings(const AppSettings& next) {
    const AppSettings prev = settings_;
    settings_ = next;
    for (size_t i = 0; i < 4; ++i)
        if (!sameInput(prev.inputs[i], next.inputs[i])) loop_->setInput(int(i), next.inputs[i]);
    if (next.toasterRoot != prev.toasterRoot || !screen_)
        buildPanel();
    else if (next.panelPalette != prev.panelPalette)
        screen_->setPanelPalette(next.panelPalette == PanelPalette::Preview);
    if (audio_ && next.audioOutDevice != prev.audioOutDevice) {
        audio_->stop();
        startAudio();
    }
    if (!settings_.save()) message(tr("Cannot save the settings to %1").arg(QDir::toNativeSeparators(settings_.file())));
    updateStatus();
}

void AppController::startAudio() {
    vt::media::AudioOutputSettings as;
    as.deviceId = settings_.audioOutDevice.toStdString();
    std::string why;
    if (!audio_->start(as, &why)) message(tr("No audio output: %1").arg(QString::fromStdString(why)));
}

// ---- shutdown ----

void AppController::shutdown() {
    if (shutDown_) return;
    shutDown_ = true;
    timer_.stop();
    if (settingsDialog_) settingsDialog_->reject();
    // 1. The recording, finished and playable.
    loop_->removeSink(recorder_.get());
    if (stopping_.valid()) stopping_.get();
    recorder_->stop();
    recording_ = false;
    // 2. The audio output.
    if (audio_) {
        loop_->removeSink(audio_.get());
        audio_->stop();
    }
    // 3. The monitors (off the loop before their windows go).
    for (MonitorWindow* m : {program_.get(), preview_.get()}) {
        loop_->removeMonitor(&m->sink());
        m->close();
    }
    // 4. The loop.
    loop_->stop();
    updateRecordAction();
    settings_.windowGeometry = window_->saveGeometry();
    settings_.save();
    emit finished();
}
