#pragma once
// Shared by the app's Qt tests: the original Switcher art and font from the Toaster data root, and
// a SwitcherActions fake.
#include <QFile>
#include <QStringList>
#include <cstdlib>

#include "controller.h"
#include "vt/assets/font.h"
#include "vt/assets/panel.h"

namespace apptest {

inline vt::assets::HunkImage hunks(const char* rel) {
    QFile f(QStringLiteral(VT_DATA_ROOT "/") + QString::fromLatin1(rel));
    if (!f.open(QIODevice::ReadOnly)) qFatal("cannot open %s", qPrintable(f.fileName()));
    const QByteArray b = f.readAll();
    return vt::assets::loadHunks({reinterpret_cast<const uint8_t*>(b.constData()), size_t(b.size())});
}

inline const vt::assets::PanelLayout& layout() {
    static const vt::assets::PanelLayout l = vt::assets::loadPanel(hunks("4.2/Programs/switcher.main"));
    return l;
}

inline const vt::assets::ColorFont& font() {
    static const auto f = vt::assets::loadColorFont(hunks("4.2/Programs/System_Support/Fonts/CGInterface/14.4C"));
    return f;
}

inline const vt::assets::PanelNode& node(const char* id) {
    for (const auto& n : layout().nodes)
        if (n.id == id) return n;
    qFatal("no node %s", id);
    std::abort();
}

inline QPoint centre(const vt::Rect& r) { return {r.x + r.w / 2, r.y + r.h / 2}; }

inline const char* busName(vt::Bus b) { return b == vt::Bus::Main ? "Main" : b == vt::Bus::Preview ? "Preview" : "Overlay"; }
inline const char* sourceName(vt::Source s) {
    static const char* const n[] = {"In1", "In2", "In3", "In4", "DV1", "DV2", "Bkg"};
    return n[int(s)];
}

// Records every call as one readable line.
class FakeActions : public SwitcherActions {
public:
    QStringList calls;
    void select(vt::Bus b, vt::Source s) override { calls << QStringLiteral("select %1 %2").arg(busName(b), sourceName(s)); }
    void take() override { calls << QStringLiteral("take"); }
    void autoTransition(vt::Direction d) override {
        calls << (d == vt::Direction::Forward ? QStringLiteral("auto Forward") : QStringLiteral("auto Reverse"));
    }
    void setSpeed(vt::Speed s) override { calls << QStringLiteral("speed %1").arg(int(s)); }
    void setTBar(int p) override { calls << QStringLiteral("tbar %1").arg(p); }
    void setBackground(vt::BkgColour c) override { calls << QStringLiteral("bkg %1").arg(int(c)); }
    void loadCrouton(int i, bool run) override { calls << QStringLiteral("crouton %1 %2").arg(i).arg(run); }
    void notImplemented(const QString& what) override { calls << QStringLiteral("notImplemented ") + what; }
};

}  // namespace apptest
