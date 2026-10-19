#pragma once
// The app's settings, kept in a QSettings INI file (%APPDATA%/vtoast_reborn/vtoast.ini), and the
// dialog that edits them (OBS-like: a list of pages on the left, the page on the right).
#include <QByteArray>
#include <QDialog>
#include <QString>
#include <array>
#include <vector>

#include "vt/media/audio_out.h"
#include "vt/media/devices.h"
#include "vt/media/input.h"
#include "vt/media/recorder.h"

enum class PanelPalette { Rgb, Preview };  // panelPalette = rgb | preview

class AppSettings {
public:
    // `file` is the INI file; the default is defaultFile(). Tests give their own.
    explicit AppSettings(QString file = defaultFile());
    static QString defaultFile();
    const QString& file() const { return file_; }

    // The Toaster data root: valid when <root>/4.2/Programs/switcher.main exists.
    static bool validToasterRoot(const QString& root);

    // Reads the file; every key that is missing, unknown or invalid (not a number, out of range,
    // not one of the listed words) is left at its default. Never throws.
    void load();
    // Writes every key, replacing the file's contents; false if the file could not be written.
    bool save() const;

    QString toasterRoot;                                // default: the build-time VT_DATA_ROOT
    std::array<vt::media::InputConfig, 4> inputs;       // inputs 1-4; options and retryMs are kept too
    QString recordingPath;                              // the output folder; default: the Movies folder
    vt::media::RecordFormat recordingFormat = vt::media::RecordFormat::H264Mkv;
    QString audioOutDevice;                             // OutputDevice::id; "" = the default device
    PanelPalette panelPalette = PanelPalette::Rgb;
    int screenZoom = 0;                                 // Switcher screen: 0 = fit the display, else 1..8
    int monitorZoom = 1;                                // Program/Preview monitors: 1..4
    bool underscan = false;                             // monitors show the whole 768 x 482 raster
    QByteArray windowGeometry;                          // the main window, QWidget::saveGeometry()

private:
    QString file_;
};

// The device lists the Inputs and Audio pages offer.
struct DeviceLists {
    std::vector<vt::media::DeviceInfo> video, audio;
    std::vector<vt::media::OutputDevice> output;
    static DeviceLists live();  // listDevices / listOutputDevices
};

class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;

// Edits the settings the pages cover (inputs, recording, audio output, Toaster data root, panel
// palette); everything else in the AppSettings given is carried through unchanged.
class SettingsDialog : public QDialog {
    Q_OBJECT
public:
    SettingsDialog(const AppSettings& current, DeviceLists devices, QWidget* parent = nullptr);
    AppSettings settings() const;  // `current` with what the pages hold now

private:
    class InputPage;
    QWidget* makeInputsPage(const DeviceLists&);
    QWidget* makeRecordingPage();
    QWidget* makeAudioPage(const DeviceLists&);
    QWidget* makeToasterPage();
    QWidget* makeDisplayPage();
    void checkRoot();  // shows whether the root is valid; OK needs a valid one

    AppSettings base_;
    std::array<InputPage*, 4> inputPages_{};
    QComboBox* format_ = nullptr;
    QLineEdit *recordingPath_ = nullptr, *root_ = nullptr;
    QComboBox *audioOut_ = nullptr, *palette_ = nullptr;
    QLabel* rootStatus_ = nullptr;
    QPushButton* ok_ = nullptr;
};
