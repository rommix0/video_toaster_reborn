#pragma once
// What the operator can ask of the Switcher from the screen (SwitcherActions; tests use a fake), and
// AppController, the app itself: it posts every action to the ProgramLoop.
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
#include "vt/switcher.h"
#include "vt/timing.h"

class SwitcherActions {
public:
    virtual ~SwitcherActions() = default;
    virtual void select(vt::Bus, vt::Source) = 0;
    virtual void take() = 0;
    virtual void autoTransition(vt::Direction) = 0;
    virtual void setSpeed(vt::Speed) = 0;
    virtual void setTBar(int position) = 0;              // 0..511
    virtual void setBackground(vt::BkgColour) = 0;
    virtual void loadCrouton(int index, bool run) = 0;   // run = also start Auto (double click)
    virtual void notImplemented(const QString& what) = 0;  // a control that does nothing in v1
};

class MainWindow;
class MixerDock;
class MonitorWindow;
class ProjectView;
class QLabel;
class QPlainTextEdit;
class ToasterScreen;
namespace vt { class Effect; }
namespace vt::media { class AudioOutput; class ProgramLoop; class Recorder; }

// Owns the ProgramLoop (built from the settings' inputs), the Recorder and the AudioOutput (both
// Program sinks), the Program and Preview monitor windows, and the main window with the Switcher
// screen, the mixer dock, the menus and the status bar.
//
// - Every action is a ProgramLoop::post; loading a crouton (vt::loadEffect; the kEffectCache most
//   recently clicked stay loaded, keyed by path) is a postChecked, so a refusal shows in the status bar (each time SwitcherSnapshot::refusalCount
//   changes) while the panel keeps showing the engine's state.
// - poll(), on a 30 Hz timer, gives switcherState() to the screen, marks the loaded crouton, drains
//   the log queue (the vt::setLogSink callback only queues: it runs under a global lock on worker
//   threads) into the status bar and the log view (Help -> Log), and updates the status labels.
// - Toaster data root: while settings.toasterRoot is not valid (or its art does not load) the
//   Switcher tab shows "Toaster data not found at <root>" with a button opening Settings on the
//   Toaster data page; a valid root chosen there builds the panel at once.
// - Recording: File -> Start Recording writes vtoast-YYYYMMDD-HHMMSS.<ext> in the recording folder;
//   Stop runs Recorder::stop() on another thread (it blocks while the queue drains), the menu
//   reading "Stopping…" until it is done.
// - Mode::Realtime (the app) starts the loop and the audio output at once. Mode::Offline (tests)
//   runs no thread and no audio device: runOffline() drives the loop, and the recorder waits for
//   queue room instead of dropping frames.
// - shutdown() (closing the main window): stops the recording (finalised), the audio output, takes
//   the monitors off the loop and stops the loop, then saves the settings.
class AppController : public QObject, public SwitcherActions {
    Q_OBJECT
public:
    enum class Mode { Realtime, Offline };
    // Loaded croutons kept (an effect can be ~90 MB). An evicted effect the engine still uses stays
    // alive through the engine's own shared_ptr.
    static constexpr size_t kEffectCache = 12;

    AppController(AppSettings&, Mode, QObject* parent = nullptr);
    ~AppController() override;  // shutdown()

    MainWindow& window() { return *window_; }
    MonitorWindow& programMonitor() { return *program_; }
    MonitorWindow& previewMonitor() { return *preview_; }
    MixerDock& mixerDock() { return *mixer_; }
    vt::media::ProgramLoop& loop() { return *loop_; }
    ToasterScreen* screen() const;     // null while the data root is not valid
    ProjectView* projectView() const;
    QPlainTextEdit* logView() const { return logView_; }
    size_t cachedEffects() const { return effects_.size(); }
    void show();  // the main window and the monitors

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
    void setBackground(vt::BkgColour) override;
    void loadCrouton(int index, bool run) override;
    void notImplemented(const QString& what) override;

signals:
    void finished();

private:
    struct LogQueue;

    void buildPanel();                       // the Switcher screen, or the data-root message
    void showDataMessage(const QString& why);
    void startAudio();
    void updateRecordAction();
    void updateStatus();
    void message(const QString&, int ms = 8000);

    AppSettings& settings_;
    const Mode mode_;
    std::unique_ptr<vt::media::ProgramLoop> loop_;
    std::unique_ptr<vt::media::Recorder> recorder_;
    std::unique_ptr<vt::media::AudioOutput> audio_;  // Realtime only
    std::unique_ptr<MonitorWindow> program_, preview_;
    std::unique_ptr<MainWindow> window_;
    MixerDock* mixer_ = nullptr;
    QPointer<ToasterScreen> screen_;
    QPointer<ProjectView> view_;
    QPointer<SettingsDialog> settingsDialog_;
    QPlainTextEdit* logView_ = nullptr;
    QLabel *inputsLabel_ = nullptr, *recordLabel_ = nullptr, *loopLabel_ = nullptr;
    QTimer timer_;
    std::shared_ptr<LogQueue> log_;

    // Loaded croutons, the most recently clicked first (at most kEffectCache); UI thread only.
    std::deque<std::pair<std::filesystem::path, std::shared_ptr<const vt::Effect>>> effects_;
    std::mutex effectMu_;
    std::filesystem::path currentEffect_;  // the crouton the loop last made current (set on the loop thread)
    uint64_t refusalsShown_ = 0;

    bool recording_ = false;
    QString recordingFile_;
    std::future<void> stopping_;   // Recorder::stop() running on another thread
    bool shutDown_ = false;
};
