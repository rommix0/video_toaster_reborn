#include "mixer_dock.h"

#include <QAbstractButton>
#include <QSlider>
#include <QtTest>
#include <cmath>
#include <memory>
#include <vector>

using vt::media::Channel;

class TestMixerDock : public QObject {
    Q_OBJECT
private slots:
    void faderMuteAndMeter();
    void faderRange();
};

// Runs one tick of constant 0.5 on input 1 (Main = In1) through the mixer.
static void tick(vt::media::AudioMixer& m) {
    std::vector<float> in(1600, 0.5f), out(1600);
    std::array<std::span<const float>, 4> inputs{std::span<const float>(in), {}, {}, {}};
    m.process(vt::media::MixState{}, inputs, nullptr, out);
}

void TestMixerDock::faderRange() {
    vt::media::AudioMixer mixer;
    MixerDock dock(mixer);
    for (int i = 0; i < MixerDock::kChannels; ++i) {
        QSlider* f = dock.fader(Channel(i));
        QCOMPARE(f->minimum(), -960);
        QCOMPARE(f->maximum(), 240);
        QCOMPARE(f->value(), 0);
        QVERIFY(!dock.muteButton(Channel(i))->isChecked());
    }
    QCOMPARE(dock.windowTitle(), QStringLiteral("Audio Mixer"));
}

// The fader and the mute reach the mixer, and the meter shows the level they leave (post-fader).
void TestMixerDock::faderMuteAndMeter() {
    vt::media::AudioMixer mixer;
    MixerDock dock(mixer);
    tick(mixer);
    tick(mixer);
    dock.poll();
    QVERIFY(std::abs(dock.meter(Channel::In1).peakDb - 20 * std::log10(0.5f)) < 0.1f);
    QVERIFY(std::abs(dock.meter(Channel::Master).peakDb - 20 * std::log10(0.5f)) < 0.1f);
    QVERIFY(std::isinf(dock.meter(Channel::In2).peakDb));  // silence

    dock.fader(Channel::In1)->setValue(-200);  // -20 dB
    tick(mixer);                                // the gain ramps over this tick
    dock.poll();
    tick(mixer);
    dock.poll();
    QVERIFY(std::abs(dock.meter(Channel::In1).peakDb - (20 * std::log10(0.5f) - 20)) < 0.1f);

    dock.muteButton(Channel::In1)->click();
    tick(mixer);
    dock.poll();
    tick(mixer);
    dock.poll();
    QVERIFY(std::isinf(dock.meter(Channel::In1).peakDb));
    QVERIFY(std::isinf(dock.meter(Channel::Master).peakDb));
}

std::unique_ptr<QObject> makeMixerDockTests() { return std::make_unique<TestMixerDock>(); }

#include "test_mixer_dock.moc"
