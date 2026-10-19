#pragma once
// The audio mixer dock: per channel (In 1-4, Effect sounds, Master) a fader (-96..+24 dB, 0 by
// default), a mute button and a level meter (peak and RMS in dBFS). The dock is the single reader
// of AudioMixer::meters(), which it polls at 30 Hz on the UI thread; the faders and mutes go to the
// mixer's atomic setters, so nothing here blocks the loop.
#include <QDockWidget>
#include <QTimer>
#include <QWidget>
#include <array>
#include <limits>

#include "vt/media/mixer.h"

class QAbstractButton;
class QLabel;
class QSlider;

// One channel's meter: an RMS bar with the peak as a line over it, on a -60..0 dBFS scale.
class MeterBar : public QWidget {
    Q_OBJECT
public:
    static constexpr float kFloorDb = -60.f;
    explicit MeterBar(QWidget* parent = nullptr);
    void setLevels(float peakDb, float rmsDb);
    QSize sizeHint() const override { return {14, 120}; }

protected:
    void paintEvent(QPaintEvent*) override;

private:
    float peakDb_ = -std::numeric_limits<float>::infinity(), rmsDb_ = peakDb_;
};

class MixerDock : public QDockWidget {
    Q_OBJECT
public:
    static constexpr int kChannels = 6;
    static constexpr float kMinDb = -96.f, kMaxDb = 24.f;

    explicit MixerDock(vt::media::AudioMixer&, QWidget* parent = nullptr);

    // Reads the mixer's meters and updates the display. The 30 Hz timer calls it.
    void poll();
    // The last reading of a channel, as the meters show it.
    vt::media::Meter meter(vt::media::Channel c) const { return levels_[size_t(c)]; }
    QSlider* fader(vt::media::Channel c) const { return strips_[size_t(c)].fader; }  // tenths of a dB
    QAbstractButton* muteButton(vt::media::Channel c) const { return strips_[size_t(c)].mute; }

private:
    struct Strip {
        QSlider* fader = nullptr;
        QAbstractButton* mute = nullptr;
        QLabel* gain = nullptr;
        QLabel* peak = nullptr;
        MeterBar* bar = nullptr;
    };

    vt::media::AudioMixer& mixer_;
    std::array<Strip, kChannels> strips_;
    std::array<vt::media::Meter, kChannels> levels_;
    QTimer timer_;
};
