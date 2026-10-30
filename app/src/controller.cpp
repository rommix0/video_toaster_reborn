#include "controller.h"

#include <QAbstractSpinBox>
#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QComboBox>
#include <QDateTime>
#include <QDialog>
#include <QDir>
#include <QFileDialog>
#include <QFile>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QStatusBar>
#include <QStyle>
#include <QTextEdit>
#include <QVBoxLayout>
#include <algorithm>
#include <deque>
#include <exception>
#include <stdexcept>
#include <string>
#include <vector>

#include "main_window.h"
#include "mixer_dock.h"
#include "position_dock.h"
#include "monitor_window.h"
#include "project_grid.h"
#include "toaster_screen.h"
#include "vt/assets/font.h"
#include "vt/assets/hunk.h"
#include "vt/assets/panel.h"
#include "vt/control.h"
#include "vt/effect.h"
#include "vt/framestore.h"
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

// vt::media::AudioOutput as the AudioDevice. A start first checks that the device is connected, so
// the retries while it is away do not log a failure every time.
class LiveAudio : public AudioDevice {
public:
    void onProgram(int64_t tick, const vt::Frame& f, std::span<const float> stereo) override { out_.onProgram(tick, f, stereo); }
    bool start(const std::string& deviceId, std::string* why) override {
        const auto devices = vt::media::listOutputDevices();
        if (std::none_of(devices.begin(), devices.end(), [&](const auto& d) { return deviceId.empty() || d.id == deviceId; })) {
            *why = deviceId.empty() ? "no playback device" : "the playback device is not connected";
            return false;
        }
        vt::media::AudioOutputSettings as;
        as.deviceId = deviceId;
        return out_.start(as, why);
    }
    void stop() override { out_.stop(); }
    bool running() const override { return out_.status().running; }

private:
    vt::media::AudioOutput out_;
};

}  // namespace

// What the log sink leaves for the UI thread.
struct AppController::LogQueue {
    std::mutex mu;
    std::deque<std::string> lines;
    uint64_t dropped = 0;  // lines that did not fit
};

AppController::AppController(AppSettings& settings, Mode mode, QObject* parent, std::unique_ptr<AudioDevice> audio)
    : QObject(parent), settings_(settings), mode_(mode), audio_(std::move(audio)), log_(std::make_shared<LogQueue>()) {
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
    if (!audio_ && mode_ == Mode::Realtime) audio_ = std::make_unique<LiveAudio>();
    if (audio_) loop_->addSink(audio_.get());

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
    position_ = new PositionDock(*this, window_.get());
    window_->addDockWidget(Qt::RightDockWidgetArea, position_);
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
    windows->addAction(position_->toggleViewAction());
    zoomGroup_ = new QActionGroup(this);
    connect(window_->zoomMenu(), &QMenu::aboutToShow, this, &AppController::fillZoomMenu);
    window_->installEventFilter(this);
    QCoreApplication::instance()->installEventFilter(this);

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
    audioLabel_ = new QLabel(tr("No audio output"), window_.get());
    audioLabel_->setObjectName(QStringLiteral("audioStatus"));
    for (QLabel* l : {audioLabel_, inputsLabel_, recordLabel_, loopLabel_}) window_->statusBar()->addPermanentWidget(l);
    audioLabel_->hide();  // while the audio output runs (or there is none)

    window_->resize(800, 500);  // until the panel sets its own size, or the stored geometry
    buildPanel();
    if (!settings_.windowGeometry.isEmpty()) window_->restoreGeometry(settings_.windowGeometry);
    fillZoomMenu();
    if (settings_.programGeometry.isEmpty() && settings_.previewGeometry.isEmpty()) placeMonitors();
    for (auto [m, g] : {std::pair{program_.get(), &settings_.programGeometry}, std::pair{preview_.get(), &settings_.previewGeometry}})
        if (!g->isEmpty() && m->restoreGeometry(*g)) m->resize(m->sizeHint());  // the place; the size follows the zoom
    updateRecordAction();
    updateStatus();

    if (mode_ == Mode::Realtime) loop_->start();
    if (audio_) startAudio();
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
    program_->show();
    preview_->show();
    window_->show();
    if (settings_.windowGeometry.isEmpty()) applyScreenZoom();  // shown: sized from its real layout
    window_->raise();
    window_->activateWindow();
    if (screen_) screen_->setFocus();
}

void AppController::placeMonitors() {
    // Frames are not known before a window is shown: estimated as 8-px borders and a title bar.
    constexpr int kBorder = 8;
    const int title = window_->style()->pixelMetric(QStyle::PM_TitleBarHeight) + kBorder;
    const QRect avail = window_->screen()->availableGeometry();
    if (settings_.windowGeometry.isEmpty()) window_->move(avail.topLeft());  // so that there is room beside it
    const QSize p = program_->sizeHint(), v = preview_->sizeHint();
    const int x = window_->x() + window_->width() + 2 * kBorder, y = window_->y();
    auto fits = [&](const QRect& r) { return avail.contains(r.adjusted(0, 0, 2 * kBorder, title + kBorder)); };
    if (fits({x, y, p.width(), p.height()}) && fits({x, y + p.height() + title + kBorder, v.width(), v.height()})) {
        program_->move(x, y);
        preview_->move(x, y + p.height() + title + kBorder);
    } else if (fits({x, y, p.width() + 2 * kBorder + v.width(), std::max(p.height(), v.height())})) {
        program_->move(x, y);
        preview_->move(x + p.width() + 2 * kBorder, y);
    }
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
        auto* view = new ProjectView(toPath(dir.filePath(QStringLiteral("4.2"))), toPath(settings_.framestorePath), screen);
        screen->setPanelPalette(settings_.panelPalette == PanelPalette::Preview);
        // The drawer list must not keep the keys (Space, Return, digits) from the screen.
        connect(view->drawerPicker(), &QComboBox::activated, screen, [screen] { screen->setFocus(); });
        window_->setSwitcherWidget(view);
        screen_ = screen;
        view_ = view;
        screen->setZoom(settings_.screenZoom > 0 ? settings_.screenZoom : defaultScreenZoom());
        screen->setState(loop_->switcherState());
        screen->setFocus();
        if (settings_.windowGeometry.isEmpty()) window_->resize(window_->sizeHint());
    } catch (const std::exception& e) {
        showDataMessage(QString::fromStdString(e.what()));
    }
}

int AppController::chromeHeight() const {
    int chrome = window_->chromeHeight();
    if (view_) chrome += view_->drawerPicker()->sizeHint().height() + view_->layout()->spacing();
    return chrome;
}

int AppController::fitScreenZoom() const {
    return screenFitZoom(*window_->screen(), chromeHeight(), window_->chromeWidth());
}

int AppController::defaultScreenZoom() const {
    return defaultZoom(*window_->screen(), chromeHeight(), window_->chromeWidth());
}

void AppController::setScreenZoom(int z) {
    settings_.screenZoom = std::clamp(z, 0, 8);
    applyScreenZoom();
}

void AppController::applyScreenZoom() {
    if (!screen_) return;
    screen_->setZoom(settings_.screenZoom > 0 ? settings_.screenZoom : defaultScreenZoom());
    if (window_->windowState() & (Qt::WindowMaximized | Qt::WindowFullScreen)) return;
    // The widgets and layouts between the screen and the window cache their size hints until the
    // posted layout requests run: drop them, so the window's hint follows the screen's at once.
    for (QWidget* w = screen_; w; w = w->parentWidget()) {
        w->updateGeometry();
        if (QLayout* l = w->layout()) l->invalidate();
    }
    window_->resize(window_->sizeHint());
    // The window's hint can be a few pixels off what its layouts then give the screen (the tab frame
    // overlaps the tab bar, depending on the style): make up the difference.
    if (window_->isVisible()) {
        window_->layout()->activate();
        window_->resize(window_->size() + screen_->sizeHint() - screen_->size());
    }
}

// Default (the windowed size), then 1x up to the zoom that fits the display (or the current zoom,
// if larger); rebuilt each time the menu opens, so it follows the display the window is on.
void AppController::fillZoomMenu() {
    QMenu* menu = window_->zoomMenu();
    menu->clear();
    const int top = std::max(fitScreenZoom(), settings_.screenZoom);
    for (int z = 0; z <= top; ++z) {
        QAction* a = menu->addAction(z ? tr("%1×").arg(z) : tr("Default"));
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

bool AppController::eventFilter(QObject* watched, QEvent* e) {
    if (e->type() == QEvent::KeyPress && forwardKey(watched, static_cast<QKeyEvent*>(e))) return true;
    // The screen snaps to the new ratio itself; with Default the window also takes the new default.
    // Queued: the window's widgets get the event too, and the layout must see their new size hints.
    if (watched == window_.get() && e->type() == QEvent::DevicePixelRatioChange)
        QMetaObject::invokeMethod(this, [this] { if (!shutDown_ && settings_.screenZoom == 0) applyScreenZoom(); },
                                  Qt::QueuedConnection);
    return QObject::eventFilter(watched, e);
}

// A key press for a widget of the main window, the mixer dock or a monitor goes to the Switcher
// screen when it is one of the Switcher's keys, without Ctrl, Alt or Meta (those are the menus').
bool AppController::forwardKey(QObject* receiver, QKeyEvent* e) {
    auto* w = qobject_cast<QWidget*>(receiver);
    if (!w || !screen_ || w == screen_ || shutDown_) return false;
    if (e->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier)) return false;
    // The widget the key was typed into: a key a text field ignores comes here again for its parents.
    QWidget* origin = QApplication::focusWidget() ? QApplication::focusWidget() : w;
    const QWidget* top = origin->window();
    if (top != window_.get() && top != mixer_ && top != position_ && top != program_.get() && top != preview_.get())
        return false;
    if (QApplication::activePopupWidget() || QApplication::activeModalWidget()) return false;
    if (qobject_cast<QLineEdit*>(origin) || qobject_cast<QAbstractSpinBox*>(origin) || qobject_cast<QTextEdit*>(origin) ||
        qobject_cast<QPlainTextEdit*>(origin))
        return false;
    return screen_->switcherKey(e);
}

// ---- actions ----

void AppController::select(vt::Bus b, vt::Source s) {
    loop_->postChecked([b, s](vt::Switcher& sw, std::string& why) { return sw.select(b, s, &why); });
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

void AppController::releaseTBar() {
    loop_->post([](vt::Switcher& sw) { sw.releaseTBar(); });
}

void AppController::movePosition(int dx, int dy) {
    loop_->post([dx, dy](vt::Switcher& sw) { sw.movePosition(dx, dy); });
}

void AppController::endPositioning(vt::PositionEnd how) {
    loop_->post([how](vt::Switcher& sw) { sw.endPositioning(how); });
}

void AppController::setKeyMode(vt::KeyMode m) {
    loop_->post([m](vt::Switcher& sw) { sw.setKeyMode(m); });
}

void AppController::setKeyLevel(int level) {
    loop_->post([level](vt::Switcher& sw) { sw.setKeyLevel(level); });
}

void AppController::stopCgScroll() {
    loop_->post([](vt::Switcher& sw) { sw.stopCgScroll(); });
}

void AppController::cgScrollIgnored() { message(tr("A scroll or crawl is playing: right-click stops it"), 3000); }

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
        // A framestore is a still for DV1/DV2 (FREEZE.md 4), not an effect: no Auto, nothing cached.
        // vt::isFramestore decides (FRAM in the file's CrUD or its icon's, or a bare DV picture).
        // Only a cache miss asks: an effect file is read twice then, but it can be large.
        // A control crouton is a Switcher move (FREEZE.md 4): the BKG colour, then Preview.
        if (const auto ctl = vt::loadControl(c.effect)) {
            if (!ctl->preview) return message(tr("%1: a project control, for the sequencer").arg(QString::fromStdString(c.name)));
            loop_->postChecked([m = *ctl](vt::Switcher& sw, std::string& why) {
                sw.clearCgPage();  // another crouton selected
                sw.clearCgScroll();
                if (m.background) sw.setBackground(*m.background);
                return sw.select(vt::Bus::Preview, *m.preview, &why);
            });
            return;
        }
        if (vt::isCgScroll(c.effect)) {
            std::string why;
            auto page = vt::loadCgScroll(c.effect, &why);
            if (!page) return message(tr("Cannot load %1: %2").arg(QString::fromStdString(c.name), QString::fromStdString(why)));
            pageHoldUntil_ = loop_->switcherState().tick + 2;  // as for a CG key page below
            loop_->postChecked([this, p = std::make_shared<const vt::CgScroll>(std::move(*page)), run](vt::Switcher& sw,
                                                                                                  std::string& why) {
                if (!sw.loadCgScroll(p, run, &why)) return false;
                const std::lock_guard l(effectMu_);
                currentEffect_ = p->path;
                currentIsPage_ = true;
                return true;
            });
            return;
        }
        if (vt::isCgKeyPage(c.effect)) {
            std::string why;
            auto page = vt::loadCgKeyPage(c.effect, &why);
            if (!page) return message(tr("Cannot load %1: %2").arg(QString::fromStdString(c.name), QString::fromStdString(why)));
            // The snapshot read now is tick T or later; the load runs at the start of tick T + 1, or
            // T + 2 if T + 1 had already taken its commands, so the snapshot of T + 2 shows it.
            pageHoldUntil_ = loop_->switcherState().tick + 2;
            loop_->postChecked([this, p = std::make_shared<const vt::CgKeyPage>(std::move(*page)), run](vt::Switcher& sw,
                                                                                                    std::string& why) {
                if (!sw.loadCgPage(p, run, &why)) return false;
                const std::lock_guard l(effectMu_);
                currentEffect_ = p->path;
                currentIsPage_ = true;
                return true;
            });
            return;
        }
        if (std::optional<vt::Frame> still; vt::isFramestore(c.effect, &still)) {
            std::string why;
            if (!still) still = vt::loadFramestore(c.effect, &why);
            if (!still) return message(tr("not a framestore: %1").arg(QString::fromStdString(why)));
            loop_->postChecked([f = std::make_shared<const vt::Frame>(std::move(*still))](vt::Switcher& sw, std::string& why) {
                return sw.loadStill(*f, &why);
            });
            return;
        }
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
            currentIsPage_ = false;
        }
        if (run) sw.autoTransition(sw.speed());
        return true;
    });
}

void AppController::setFrozen(bool frozen) {
    loop_->postChecked([frozen](vt::Switcher& sw, std::string& why) { return sw.setFrozen(frozen, &why); });
}

void AppController::freezeKey(bool frozen) {
    loop_->postChecked([frozen](vt::Switcher& sw, std::string& why) { return sw.freezeKey(frozen, &why); });
}

void AppController::grab(int input) {
    loop_->postChecked([input](vt::Switcher& sw, std::string& why) { return sw.grab(input, &why); });
}

// The refusals saveFramestore(path) posts, checked first from the last snapshot so no dialog opens
// for a save that cannot happen (the posted save checks again).
void AppController::saveFramestore() {
    const vt::media::SwitcherSnapshot s = loop_->switcherState();
    if (s.transition && !s.overlayEngaged) return message(tr("Refused: finish the effect first"));
    if (!s.hasStill) return message(tr("Refused: SAVE: no still in DV1/DV2"));
    QDir().mkpath(settings_.framestorePath);
    const QString file = QFileDialog::getSaveFileName(window_.get(), tr("Save framestore"), settings_.framestorePath);
    if (!file.isEmpty()) saveFramestore(file);
}

// The still is the one saveSource picks. The file is written on the loop thread; the status message
// and the drawer rescan go back to the UI thread.
// ponytail: writing on the loop thread, so a frame may drop on SAVE; move it to a worker if that shows.
bool AppController::saveFramestore(const QString& path) {
    if (path.isEmpty()) return false;
    const fs::path file = toPath(path);
    loop_->postChecked([this, file](vt::Switcher& sw, std::string& why) {
        if (sw.transitionActive() && !sw.overlayEngaged()) {
            why = "finish the effect first";
            return false;
        }
        const auto dv = sw.saveSource(&why);
        if (!dv) return false;
        if (!vt::saveFramestore(file, sw.still(*dv), &why)) return false;
        const QString name = QString::fromStdU16String(file.filename().generic_u16string());
        QMetaObject::invokeMethod(
            this,
            [this, name] {
                if (view_) view_->rescan();
                message(tr("Saved %1").arg(name));
            },
            Qt::QueuedConnection);
        return true;
    });
    return true;
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

    if (audio_ && !audio_->running() && !shutDown_) {
        audioLabel_->show();
        if (audioTried_.elapsed() >= kAudioRetryMs) startAudio();
    }

    const vt::media::SwitcherSnapshot s = loop_->switcherState();
    if (s.refusalCount != refusalsShown_) {
        refusalsShown_ = s.refusalCount;
        if (!s.lastRefusal.empty()) message(tr("Refused: %1").arg(QString::fromStdString(s.lastRefusal)));
    }
    position_->setState(s);
    if (screen_) {
        screen_->setState(s);
        fs::path current;
        {
            const std::lock_guard l(effectMu_);
            current = currentEffect_;
            // The page has gone (the Fade is left), judged only from a snapshot taken after its load.
            if (currentIsPage_ && s.cgPage.dv == vt::Source::None && !s.cgScroll.loaded && s.tick >= pageHoldUntil_)
                current.clear();
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
    if (next.toasterRoot != prev.toasterRoot || !screen_) {
        buildPanel();
    } else {
        if (next.framestorePath != prev.framestorePath) view_->setSavedFolder(toPath(next.framestorePath));
        if (next.panelPalette != prev.panelPalette) screen_->setPanelPalette(next.panelPalette == PanelPalette::Preview);
    }
    if (audio_ && (next.audioOutDevice != prev.audioOutDevice || !audio_->running())) {
        audio_->stop();
        startAudio();
    }
    if (!settings_.save()) message(tr("Cannot save the settings to %1").arg(QDir::toNativeSeparators(settings_.file())));
    updateStatus();
}

// Says why only for the first failure in a row; the label stays while the output does not run.
void AppController::startAudio() {
    audioTried_.start();
    std::string why;
    if (audio_->start(settings_.audioOutDevice.toStdString(), &why)) {
        audioLabel_->hide();
        return;
    }
    if (audioLabel_->isHidden()) message(tr("No audio output: %1").arg(QString::fromStdString(why)));
    audioLabel_->setToolTip(QString::fromStdString(why));
    audioLabel_->show();
}

// ---- shutdown ----

void AppController::shutdown() {
    if (shutDown_) return;
    shutDown_ = true;
    timer_.stop();
    QCoreApplication::instance()->removeEventFilter(this);
    window_->removeEventFilter(this);
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
    settings_.programGeometry = program_->saveGeometry();
    settings_.previewGeometry = preview_->saveGeometry();
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
