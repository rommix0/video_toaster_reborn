#pragma once
// What the operator can ask of the Switcher from the screen (SwitcherActions; tests use a fake), and
// AppController, the app itself: it posts every action to the ProgramLoop.
#include <QElapsedTimer>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QTimer>
#include <filesystem>
#include <deque>
#include <future>
#include <memory>
#include <mutex>
#include <utility>

#include "settings.h"
#include "vt/background.h"
#include "vt/media/program_loop.h"
#include "vt/framestore.h"
#include "vt/switcher.h"
#include "vt/timing.h"

class SwitcherActions {
public:
    virtual ~SwitcherActions() = default;
    virtual void select(vt::Bus, vt::Source) = 0;        // a refusal (DV2 alone on Superimpose) shows in the status bar
    virtual void take() = 0;
    virtual void autoTransition(vt::Direction) = 0;
    virtual void setSpeed(vt::Speed) = 0;
    virtual void setTBar(int position) = 0;              // 0..511
    virtual void releaseTBar() = 0;                      // a T-bar drag ends (a Positionable effect may start positioning)
    virtual void movePosition(int dx, int dy) = 0;       // positioning: mouse counts, right / down +
    virtual void endPositioning(vt::PositionEnd) = 0;    // positioning: Space / Return, left click, right button
    virtual void setBackground(vt::BkgColour) = 0;
    virtual void loadCrouton(int index, bool run) = 0;   // run = also start Auto (double click)
    virtual void setFrozen(bool frozen) = 0;             // FREEZE / LIVE buttons
    virtual void freezeKey(bool frozen) = 0;             // keys f / l (refused while a CG key page is loaded)
    virtual void grab(int input) = 0;                    // grab buttons 1-4: 0..3
    virtual void saveFramestore(vt::SaveFields) = 0;     // SAVE's pop-up
    virtual void stabilize(vt::StabilizeFields) = 0;     // STABILIZE's pop-up, Alt+f = 2 Fields
    virtual void setKeyMode(vt::KeyMode) = 0;            // OFF / Black key / White key, keys o [ ]
    virtual void setKeyLevel(int level) = 0;             // the level box drag: 0..257
    virtual void stopCgScroll() = 0;                     // the right button while a CG scroll or crawl plays
    virtual void cgScrollIgnored() = 0;                  // any other input then: the status note
};

// The live audio output as the controller drives it: a Program sink that plays on an output device.
// The app uses vt::media::AudioOutput; tests give a stand-in.
class AudioDevice : public vt::media::ProgramSink {
public:
    virtual bool start(const std::string& deviceId, std::string* why) = 0;  // "" = the default device
    virtual void stop() = 0;
    virtual bool running() const = 0;  // false after a failed start or once the device went away
};

class MainWindow;
class QActionGroup;
class QKeyEvent;
class MixerDock;
class PositionDock;
class MonitorWindow;
class ProjectView;
class QLabel;
class QPlainTextEdit;
class ToasterScreen;
namespace vt { class Effect; }
namespace vt::media { class Recorder; }

// Owns the ProgramLoop (built from the settings' inputs), the Recorder and the AudioOutput (both
// Program sinks), the Program and Preview monitor windows, and the main window with the Switcher
// screen, the mixer dock, the menus and the status bar.
//
// - Every action is a ProgramLoop::post; loading a crouton (vt::loadEffect; the kEffectCache most
//   recently clicked stay loaded, keyed by path) is a postChecked, so a refusal shows in the status bar (each time SwitcherSnapshot::refusalCount
//   changes) while the panel keeps showing the engine's state. FREEZE, LIVE and the grab buttons are
//   postChecked too: the engine refuses them during a transition or an engaged ChromaFX.
// - A FRAM crouton (the Framestore, CG/Frames and "Saved framestores" drawers) loads as a still into
//   DV1/DV2 (vt::loadFramestore, then a postChecked loadStill); a double click does not run Auto.
// - A CG key page (KEYF, the CG/Keys drawer) is decoded on every click (vt::loadCgKeyPage, never
//   cached) and loaded with a postChecked loadCgPage; a double click also cuts it on. It is the loaded
//   crouton until the engine clears it (its off-dissolve ends, or another crouton is loaded; a
//   control crouton clears it first). The keys f / l go to freezeKey, which a loaded page refuses.
// - A CG scroll or crawl page (SCRO / CRAW, the CG/Scrolls and CG/Crawls drawers) is decoded and
//   pre-rendered on this thread (vt::loadCgScroll; the last one is kept while its file is unchanged, so a
//   double click decodes once) and loaded with a postChecked loadCgScroll; a double click also plays it,
//   and the screen counts it as playing from then on (holdScrollPlaying). While it plays the screen sends
//   only stopCgScroll (the right button); any other input calls cgScrollIgnored, a status note.
// - An uncached crouton's TYPE is read once (vt::croutonType) to pick a page loader.
//   SAVE asks for a file in settings.framestorePath and posts the write of the still saveSource picks;
//   the status bar says "Saved <name>" and the "Saved framestores" drawer is re-read.
// - poll(), on a 30 Hz timer, gives switcherState() to the screen, marks the loaded crouton, drains
//   the log queue (the vt::setLogSink callback only queues: it runs under a global lock on worker
//   threads) into the status bar and the log view (Help -> Log), and updates the status labels.
// - The main window resizes freely; the Switcher screen's panel snaps to the largest whole zoom that
//   fits it. Its preferred zoom (the window's size at start-up without a stored geometry) is
//   settings.screenZoom, or with 0 the windowed default (defaultZoom: about one Toaster pixel per
//   logical screen pixel, never more than fits the main window's display beside the mixer dock);
//   View -> Zoom offers Default and 1x up to the largest that fits (or the stored zoom, if larger),
//   checks the stored choice, and applies a choice at once, resizing the window so the panel is at
//   that zoom (unless it is maximised or full screen). When the main window's device pixel ratio
//   changes (another display), the panel snaps anew; with Default the window also takes the new
//   default's size.
// - Keys: the Switcher's key map works while the main window (with the mixer dock) or a monitor is
//   the active window: an application event filter hands the mapped key presses meant for any of
//   their widgets to the Switcher screen, unless a text field has the focus or a pop-up or modal
//   dialog is open. show() shows the monitors first and the main window last, activated.
// - The monitor windows' geometry is stored in the settings; on the first launch they are placed to
//   the right of the main window, Program above Preview, when that fits on its display.
// - Toaster data root: while settings.toasterRoot is not valid (or its art does not load) the
//   Switcher tab shows "Toaster data not found at <root>" with a button opening Settings on the
//   Toaster data page; a valid root chosen there builds the panel at once.
// - Recording: File -> Start Recording writes vtoast-YYYYMMDD-HHMMSS.<ext> in the recording folder;
//   Stop runs Recorder::stop() on another thread (it blocks while the queue drains), the menu
//   reading "Stopping…" until it is done.
// - Mode::Realtime (the app) starts the loop and the audio output at once. Mode::Offline (tests)
//   runs no thread and no audio device (unless given a stand-in): runOffline() drives the loop, and
//   the recorder waits for queue room instead of dropping frames.
// - Audio output: while it is not running (the start failed, or the device went away, e.g. an
//   unplugged headset) the status bar shows "No audio output" and poll() tries to start it again
//   every kAudioRetryMs; applying the settings restarts it whenever it is not running.
// - shutdown() (closing the main window): stops the recording (finalised), the audio output, takes
//   the monitors off the loop and stops the loop, then saves the settings (with the window geometries).
class AppController : public QObject, public SwitcherActions {
    Q_OBJECT
public:
    enum class Mode { Realtime, Offline };
    // Loaded croutons kept (an effect can be ~90 MB). An evicted effect the engine still uses stays
    // alive through the engine's own shared_ptr.
    static constexpr size_t kEffectCache = 12;
    static constexpr int kAudioRetryMs = 2000;

    // `audio` replaces the audio output (Realtime: vt::media::AudioOutput; Offline: none).
    AppController(AppSettings&, Mode, QObject* parent = nullptr, std::unique_ptr<AudioDevice> audio = nullptr);
    ~AppController() override;  // shutdown()

    MainWindow& window() { return *window_; }
    MonitorWindow& programMonitor() { return *program_; }
    MonitorWindow& previewMonitor() { return *preview_; }
    MixerDock& mixerDock() { return *mixer_; }
    PositionDock& positionDock() { return *position_; }
    vt::media::ProgramLoop& loop() { return *loop_; }
    ToasterScreen* screen() const;     // null while the data root is not valid
    ProjectView* projectView() const;
    QPlainTextEdit* logView() const { return logView_; }
    size_t cachedEffects() const { return effects_.size(); }
    void show();  // the monitors, then the main window, activated
    void setScreenZoom(int);  // 0 = the windowed default; writes settings.screenZoom and applies it

    void runOffline(int64_t ticks);  // Mode::Offline: that many ticks, then poll()
    void poll();

    void startRecording();
    void stopRecording();            // returns at once; the file is finished on another thread
    bool recording() const { return recording_; }
    QString recordingFile() const { return recordingFile_; }  // the current or last one

    void openSettings(SettingsDialog::Page = SettingsDialog::InputsPage);  // non-modal; OK applies
    void applySettings(const AppSettings&);  // and saves them
    void shutdown();                         // once; then emits finished()

    // SwitcherActions
    void select(vt::Bus, vt::Source) override;
    void take() override;
    void autoTransition(vt::Direction) override;
    void setSpeed(vt::Speed) override;
    void setTBar(int position) override;
    void releaseTBar() override;
    void movePosition(int dx, int dy) override;
    void endPositioning(vt::PositionEnd) override;
    void setBackground(vt::BkgColour) override;
    void loadCrouton(int index, bool run) override;
    void setFrozen(bool frozen) override;
    void freezeKey(bool frozen) override;
    void grab(int input) override;
    void saveFramestore(vt::SaveFields) override;  // asks for a file
    void stabilize(vt::StabilizeFields) override;  // 2 Fields says why the picture stays
    void setKeyMode(vt::KeyMode) override;
    void setKeyLevel(int level) override;
    void stopCgScroll() override;
    void cgScrollIgnored() override;
    bool saveFramestore(const QString& path, vt::SaveFields);  // posts the save to `path`; false: no path

    // The main window's DevicePixelRatioChange, and the key presses of the whole application (see
    // forwardKey).
    bool eventFilter(QObject*, QEvent*) override;

signals:
    void finished();

private:
    struct LogQueue;

    void buildPanel();                       // the Switcher screen, or the data-root message
    void showDataMessage(const QString& why);
    int chromeHeight() const;      // the main window's chrome above and below the screen, logical px
    int fitScreenZoom() const;     // the largest zoom that fits the main window's display and chrome
    int defaultScreenZoom() const; // the windowed default for that display (defaultZoom)
    void applyScreenZoom();        // the preferred zoom to settings.screenZoom (or the default), the window to it
    void fillZoomMenu();
    bool forwardKey(QObject* receiver, QKeyEvent*);
    void placeMonitors();          // first launch: beside the main window, where there is room
    void startAudio();
    void updateRecordAction();
    void updateStatus();
    void message(const QString&, int ms = 8000);

    AppSettings& settings_;
    const Mode mode_;
    std::unique_ptr<vt::media::ProgramLoop> loop_;
    std::unique_ptr<vt::media::Recorder> recorder_;
    std::unique_ptr<AudioDevice> audio_;  // Realtime, or a test's stand-in
    QElapsedTimer audioTried_;            // since the last start attempt
    std::unique_ptr<MonitorWindow> program_, preview_;
    std::unique_ptr<MainWindow> window_;
    MixerDock* mixer_ = nullptr;
    PositionDock* position_ = nullptr;
    QPointer<ToasterScreen> screen_;
    QPointer<ProjectView> view_;
    QPointer<SettingsDialog> settingsDialog_;
    QPlainTextEdit* logView_ = nullptr;
    QLabel *inputsLabel_ = nullptr, *recordLabel_ = nullptr, *loopLabel_ = nullptr, *audioLabel_ = nullptr;
    QActionGroup* zoomGroup_ = nullptr;
    QTimer timer_;
    std::shared_ptr<LogQueue> log_;

    // Loaded croutons, the most recently clicked first (at most kEffectCache); UI thread only.
    std::deque<std::pair<std::filesystem::path, std::shared_ptr<const vt::Effect>>> effects_;
    std::mutex effectMu_;
    std::filesystem::path currentEffect_;  // the crouton the loop last made current (set on the loop thread)
    bool currentIsPage_ = false;           // ... a CG key page: marked only while the engine has it loaded
    int64_t pageHoldUntil_ = -1;           // a page load posted: its mark holds until a snapshot this new (UI thread)
    std::shared_ptr<const vt::CgScroll> scrollCache_;  // the last scroll or crawl page decoded (UI thread)
    std::filesystem::file_time_type scrollCacheTime_;  // ... its file's time then
    uint64_t refusalsShown_ = 0;

    bool recording_ = false;
    QString recordingFile_;
    std::future<void> stopping_;   // Recorder::stop() running on another thread
    bool shutDown_ = false;
};
