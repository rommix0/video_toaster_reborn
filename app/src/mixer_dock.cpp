#include "mixer_dock.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPushButton>
#include <QSlider>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>

using vt::media::Channel;

namespace {

constexpr const char* kNames[MixerDock::kChannels] = {"In 1", "In 2", "In 3", "In 4", "Effects", "Master"};

QString dbText(float db, int decimals = 1) {
    return std::isinf(db) ? QStringLiteral("-inf") : QString::number(double(db), 'f', decimals);
}

}  // namespace

MeterBar::MeterBar(QWidget* parent) : QWidget(parent) {
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
}

void MeterBar::setLevels(float peakDb, float rmsDb) {
    if (peakDb == peakDb_ && rmsDb == rmsDb_) return;
    peakDb_ = peakDb;
    rmsDb_ = rmsDb;
    update();
}

void MeterBar::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.fillRect(rect(), QColor(24, 24, 24));
    // Height of a level: 0 at kFloorDb (and for silence), the full bar at 0 dBFS and above.
    auto top = [this](float db) {
        const float f = std::isnan(db) ? 0.f : std::clamp((db - kFloorDb) / -kFloorDb, 0.f, 1.f);
        return height() - int(std::lround(f * height()));
    };
    auto colour = [](float db) { return db >= -3.f ? QColor(220, 60, 50) : db >= -12.f ? QColor(230, 200, 60) : QColor(70, 190, 90); };
    p.fillRect(QRect(0, top(rmsDb_), width(), height() - top(rmsDb_)), colour(rmsDb_));
    p.fillRect(QRect(0, top(peakDb_), width(), 2), colour(peakDb_));
}

MixerDock::MixerDock(vt::media::AudioMixer& mixer, QWidget* parent) : QDockWidget(tr("Audio Mixer"), parent), mixer_(mixer) {
    setObjectName(QStringLiteral("mixerDock"));
    levels_.fill({});

    auto* body = new QWidget(this);
    auto* row = new QHBoxLayout(body);
    for (int i = 0; i < kChannels; ++i) {
        const Channel ch = Channel(i);
        Strip& s = strips_[size_t(i)];
        auto* col = new QVBoxLayout;

        auto* name = new QLabel(tr(kNames[i]), body);
        name->setAlignment(Qt::AlignCenter);
        col->addWidget(name);

        auto* faderRow = new QHBoxLayout;
        s.fader = new QSlider(Qt::Vertical, body);
        s.fader->setRange(int(kMinDb * 10), int(kMaxDb * 10));  // tenths of a dB
        s.fader->setSingleStep(10);
        s.fader->setPageStep(60);
        s.fader->setValue(0);
        s.fader->setToolTip(tr("%1 fader (dB)").arg(tr(kNames[i])));
        s.bar = new MeterBar(body);
        faderRow->addWidget(s.fader, 0, Qt::AlignHCenter);
        faderRow->addWidget(s.bar);
        col->addLayout(faderRow, 1);

        s.gain = new QLabel(QStringLiteral("0.0"), body);
        s.gain->setAlignment(Qt::AlignCenter);
        s.peak = new QLabel(QStringLiteral("-inf"), body);
        s.peak->setAlignment(Qt::AlignCenter);
        s.peak->setToolTip(tr("Peak (dBFS)"));
        col->addWidget(s.gain);
        col->addWidget(s.peak);

        auto* mute = new QPushButton(tr("Mute"), body);
        mute->setCheckable(true);
        mute->setFocusPolicy(Qt::NoFocus);
        s.mute = mute;
        col->addWidget(mute);
        row->addLayout(col);

        connect(s.fader, &QSlider::valueChanged, this, [this, ch, &s](int tenths) {
            mixer_.setFaderDb(ch, float(tenths) / 10.f);
            s.gain->setText(dbText(float(tenths) / 10.f));
        });
        connect(mute, &QPushButton::toggled, this, [this, ch](bool on) { mixer_.setMute(ch, on); });
    }
    setWidget(body);

    connect(&timer_, &QTimer::timeout, this, &MixerDock::poll);
    timer_.start(1000 / 30);
}

void MixerDock::poll() {
    levels_ = mixer_.meters();
    for (size_t i = 0; i < size_t(kChannels); ++i) {
        strips_[i].bar->setLevels(levels_[i].peakDb, levels_[i].rmsDb);
        strips_[i].peak->setText(dbText(levels_[i].peakDb));
    }
}
