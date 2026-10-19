#include "toaster_screen.h"

#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace {

constexpr int kTbarMax = 511;
constexpr int kTbarImages = kTbarMax / 6 + 1;  // 86

QRect toQRect(const vt::Rect& r) { return {r.x, r.y, r.w, r.h}; }

QList<QRgb> colourTable(const vt::assets::Palette& p) {
    QList<QRgb> t;
    for (const auto& c : p) t << qRgb(c.r, c.g, c.b);
    return t;
}

// Exact pixel replication of rect of src into dst: each Toaster pixel becomes zoom x zoom device
// pixels. Any future pixel-aspect option belongs here (and in the Toaster <-> logical mappings).
void replicate(const vt::assets::IndexedImage& src, const QRect& rect, int zoom, const QList<QRgb>& colours,
               QImage& dst) {
    std::vector<QRgb> row(size_t(rect.width()) * size_t(zoom));
    for (int y = rect.top(); y <= rect.bottom(); ++y) {
        const uint8_t* in = &src.px[size_t(y) * size_t(src.w) + size_t(rect.left())];
        for (int x = 0; x < rect.width(); ++x) std::fill_n(row.begin() + x * zoom, zoom, colours[in[x]]);
        for (int dy = 0; dy < zoom; ++dy)
            std::memcpy(reinterpret_cast<QRgb*>(dst.scanLine(y * zoom + dy)) + rect.left() * zoom, row.data(),
                        row.size() * sizeof(QRgb));
    }
}

// Bus rows: node id prefix, and the button suffixes in Source order (In1..In4, DV1, DV2, BKG).
constexpr const char* kSourceSuffix[] = {"1", "2", "3", "4", "dv1", "dv2", "bkg"};
struct BusRow { const char* prefix; vt::Bus bus; };
constexpr BusRow kBusRows[] = {{"main_", vt::Bus::Main}, {"prev_", vt::Bus::Preview}, {"sup_", vt::Bus::Overlay}};

// Drawn and pressable, but nothing behind them in v1.
struct Stub { const char* id; const char* name; };
constexpr Stub kStubs[] = {
    {"fs_freeze", "FREEZE"}, {"fs_live", "LIVE"}, {"fs_1", "Framestore 1"}, {"fs_2", "Framestore 2"},
    {"fs_3", "Framestore 3"}, {"fs_4", "Framestore 4"}, {"fs_stabilize", "STABILIZE"}, {"fs_save", "SAVE"},
    {"key_off", "Key OFF"}, {"key_black", "Black key"}, {"key_white", "White key"}, {"key_level_popup", "Key level"},
};

constexpr int kEdge = 3;       // outline + 2-px bevel
constexpr int kTextLeft = 6;   // as the BKG pop-up's own text: Move(x+6, ...)

}  // namespace

BkgPopup::BkgPopup(const vt::assets::ColorFont& font, const std::vector<std::string>& names, int minWidth,
                   std::function<void(vt::BkgColour)> chosen, QWidget* parent)
    : QWidget(parent, Qt::Popup), font_(font), count_(int(std::min<size_t>(names.size(), 9))), itemH_(font.ySize + 2),
      chosen_(std::move(chosen)) {
    std::vector<std::string> trimmed;
    int textW = 0;
    for (int i = 0; i < count_; ++i) {
        const QString name = QString::fromStdString(names[size_t(i)]).trimmed();
        trimmed.push_back(name.toStdString());
        textW = std::max(textW, vt::assets::textWidth(font, trimmed.back()));
    }
    const int w = std::max(minWidth, kTextLeft + textW + kTextLeft), h = 2 * kEdge + count_ * itemH_;
    art_ = {w, h, std::vector<uint8_t>(size_t(w) * size_t(h), 2)};  // face
    auto set = [&](int x, int y, uint8_t c) { art_.px[size_t(y) * size_t(w) + size_t(x)] = c; };
    for (int i = 1; i <= 2; ++i) {           // light bevel top/left, then shadow bottom/right over it
        for (int x = 1; x < w - 1; ++x) set(x, i, 3);
        for (int y = 1; y < h - 1; ++y) set(i, y, 3);
    }
    for (int i = 1; i <= 2; ++i) {
        for (int x = i; x < w - 1; ++x) set(x, h - 1 - i, 1);
        for (int y = i; y < h - 1; ++y) set(w - 1 - i, y, 1);
    }
    for (int x = 0; x < w; ++x) set(x, 0, 0), set(x, h - 1, 0);
    for (int y = 0; y < h; ++y) set(0, y, 0), set(w - 1, y, 0);
    for (int i = 0; i < count_; ++i) {
        const QPoint pen = textPen(i);
        vt::assets::drawText(art_, font, pen.x(), pen.y(), trimmed[size_t(i)], true, 0);
    }
}

QRect BkgPopup::itemRect(int i) const { return {kEdge, kEdge + i * itemH_, art_.w - 2 * kEdge, itemH_}; }

QPoint BkgPopup::textPen(int i) const { return {kTextLeft, kEdge + i * itemH_ + 1 + font_.baseline}; }

void BkgPopup::open(const QPoint& globalTopLeft, int zoom, const QList<QRgb>& colours) {
    zoom_ = zoom;
    scaled_ = QImage(art_.w * zoom, art_.h * zoom, QImage::Format_RGB32);
    replicate(art_, QRect(0, 0, art_.w, art_.h), zoom, colours, scaled_);
    const double dpr = parentWidget() ? parentWidget()->devicePixelRatioF() : devicePixelRatioF();
    setFixedSize(int(std::ceil(art_.w * zoom / dpr)), int(std::ceil(art_.h * zoom / dpr)));
    move(globalTopLeft);
    show();
}

void BkgPopup::paintEvent(QPaintEvent*) {
    QPainter p(this);
    scaled_.setDevicePixelRatio(devicePixelRatioF());  // device pixels, copied 1:1
    p.drawImage(QPointF(0, 0), scaled_);
}

void BkgPopup::mousePressEvent(QMouseEvent* e) {
    const QPointF pos = e->position() * devicePixelRatioF() / zoom_;
    const QPoint px(int(std::floor(pos.x())), int(std::floor(pos.y())));
    if (e->button() == Qt::LeftButton)
        for (int i = 0; i < count_; ++i)
            if (itemRect(i).contains(px)) {
                close();
                chosen_(vt::BkgColour(i + 1));
                return;
            }
    QWidget::mousePressEvent(e);  // closes a popup when the click is outside it
}

void BkgPopup::keyPressEvent(QKeyEvent* e) {
    if (e->key() == Qt::Key_Escape) close();
    else QWidget::keyPressEvent(e);
}

ToasterScreen::ToasterScreen(const vt::assets::PanelLayout& layout, const vt::assets::ColorFont& cgInterface14,
                             SwitcherActions& actions, QWidget* parent)
    : QWidget(parent), layout_(layout), font_(cgInterface14), actions_(actions) {
    setFocusPolicy(Qt::StrongFocus);
    setAttribute(Qt::WA_OpaquePaintEvent);

    const auto& nodes = layout_.nodes;
    look_.assign(nodes.size(), -1);
    selected_.resize(nodes.size());
    for (size_t i = 0; i < nodes.size(); ++i)
        if (!nodes[i].hiddenByDefault && !nodes[i].image.px.empty()) drawn_.push_back(int(i));
    auto visible = [&](int i) { return i >= 0 && std::find(drawn_.begin(), drawn_.end(), i) != drawn_.end(); };
    auto control = [&](const char* id, Control c) {
        c.node = nodeIndex(id);
        if (!visible(c.node)) return;
        if (c.kind != Control::Bkg && !nodes[size_t(c.node)].states.empty())
            selected_[size_t(c.node)] = vt::assets::selectedLook(nodes[size_t(c.node)], 0);
        controls_.push_back(c);
    };
    for (const auto& row : kBusRows)
        for (int s = 0; s < 7; ++s)
            control((std::string(row.prefix) + kSourceSuffix[s]).c_str(),
                    {Control::Bus, -1, row.bus, vt::Source(s)});
    control("take", {Control::Take, -1});
    control("auto", {Control::Auto, -1});
    control("speed_s", {Control::Speed, -1, {}, {}, vt::Speed::Slow});
    control("speed_m", {Control::Speed, -1, {}, {}, vt::Speed::Medium});
    control("speed_f", {Control::Speed, -1, {}, {}, vt::Speed::Fast});
    control("bkg_popup", {Control::Bkg, -1});
    for (const auto& s : kStubs) control(s.id, {Control::Stub, -1, {}, {}, {}, QString::fromLatin1(s.name)});

    tbarNode_ = nodeIndex("tbar");
    if (!visible(tbarNode_)) tbarNode_ = -1;
    bkgNode_ = nodeIndex("bkg_popup");
    for (size_t i = 0; i < layout_.texts.size(); ++i)
        if (layout_.texts[i].id == "bkg_popup_text") bkgText_ = int(i);

    // Everything compose() and imageOf() index must be there.
    auto check = [](bool ok, const std::string& what) {
        if (!ok) throw std::runtime_error("ToasterScreen: " + what);
    };
    auto fits = [](const vt::assets::IndexedImage& img, const vt::Rect& r) {
        return img.w == r.w && img.h == r.h && img.px.size() == size_t(r.w) * size_t(r.h);
    };
    for (int i : drawn_) {
        const auto& n = nodes[size_t(i)];
        check(fits(n.image, n.rect), "node " + n.id + ": image does not match its rect");
        check(selected_[size_t(i)].px.empty() || fits(selected_[size_t(i)], n.rect),
              "node " + n.id + ": selected look does not match its rect");
    }
    if (tbarNode_ >= 0) {
        check(layout_.tbarImages.size() >= size_t(kTbarImages), "the T-bar has fewer than 86 images");
        for (const auto& img : layout_.tbarImages)
            check(fits(img, nodes[size_t(tbarNode_)].rect), "a T-bar image does not match the T-bar rect");
    }

    if (bkgNode_ >= 0)
        bkgPopup_ = new BkgPopup(font_, layout_.bkgNames, nodes[size_t(bkgNode_)].rect.w,
                                 [this](vt::BkgColour c) { actions_.setBackground(c); }, this);

    colours_ = colourTable(layout_.rgbMode);
    screen_ = {layout_.screenW, layout_.screenH, std::vector<uint8_t>(size_t(layout_.screenW) * size_t(layout_.screenH))};
    scaled_ = QImage(layout_.screenW, layout_.screenH, QImage::Format_RGB32);
    for (int i : drawn_) look_[size_t(i)] = lookOf(i);
    if (bkgText_ >= 0) layout_.texts[size_t(bkgText_)].text = bkgText();
    redraw(QRect(0, 0, layout_.screenW, layout_.screenH));
}

int ToasterScreen::nodeIndex(const char* id) const {
    for (size_t i = 0; i < layout_.nodes.size(); ++i)
        if (layout_.nodes[i].id == id) return int(i);
    return -1;
}

void ToasterScreen::setZoom(int z) {
    z = std::max(1, z);
    if (z == zoom_) return;
    zoom_ = z;
    rescaleAll();
    updateGeometry();
}

void ToasterScreen::setPanelPalette(bool previewMode) {
    colours_ = colourTable(previewMode ? layout_.previewMode : layout_.rgbMode);
    rescaleAll();
}

void ToasterScreen::setState(const vt::media::SwitcherSnapshot& s) {
    state_ = s;
    refresh();
}

QImage ToasterScreen::renderIndexedForTest() const {
    QImage img(screen_.w, screen_.h, QImage::Format_Indexed8);
    img.setColorTable(colours_);
    for (int y = 0; y < screen_.h; ++y) std::memcpy(img.scanLine(y), &screen_.px[size_t(y) * size_t(screen_.w)], size_t(screen_.w));
    return img;
}

QSize ToasterScreen::sizeHint() const {
    const double dpr = devicePixelRatioF();
    return {int(std::ceil(layout_.screenW * zoom_ / dpr)), int(std::ceil(layout_.screenH * zoom_ / dpr))};
}

int ToasterScreen::lookOf(int node) const {
    if (node == tbarNode_)  // the handler 0x5e000: state = position / 6; always the engine's (Ruling 8)
        return std::clamp(state_.tbar, 0, kTbarMax) / 6;
    if (node == pressed_) return 1;
    for (const auto& c : controls_)
        if (c.node == node) {
            if (c.kind == Control::Bus) {
                const vt::Source on = c.bus == vt::Bus::Main ? state_.main
                                      : c.bus == vt::Bus::Preview ? state_.preview : state_.overlay;
                return c.source == on;
            }
            if (c.kind == Control::Speed) return c.speed == state_.speed;  // Variable: none of S/M/F
            return 0;
        }
    return 0;
}

const vt::assets::IndexedImage& ToasterScreen::imageOf(int node) const {
    const int look = look_[size_t(node)];
    if (node == tbarNode_) return layout_.tbarImages[size_t(look)];
    if (look == 1 && !selected_[size_t(node)].px.empty()) return selected_[size_t(node)];
    return layout_.nodes[size_t(node)].image;
}

// The pop-up names a preset background; a custom colour word has no name, so it shows none.
std::string ToasterScreen::bkgText() const {
    const auto& b = state_.background;
    const size_t i = size_t(b.preset) - 1;
    return !b.custom && i < layout_.bkgNames.size() ? layout_.bkgNames[i] : std::string();
}

QRect ToasterScreen::bkgTextRect(const std::string& text) const {
    const auto& t = layout_.texts[size_t(bkgText_)];
    const int top = t.baseline ? t.y - font_.baseline : t.y;
    return {t.x, top, vt::assets::textWidth(font_, text), font_.ySize};
}

void ToasterScreen::refresh() {
    QRegion dirty;
    for (int i : drawn_) {
        const int want = lookOf(i);
        if (want != look_[size_t(i)]) {
            look_[size_t(i)] = want;
            dirty += toQRect(layout_.nodes[size_t(i)].rect);
        }
    }
    if (bkgText_ >= 0) {
        auto& text = layout_.texts[size_t(bkgText_)].text;
        if (const std::string want = bkgText(); want != text) {
            dirty += bkgTextRect(text);
            dirty += bkgTextRect(want);
            if (bkgNode_ >= 0) dirty += toQRect(layout_.nodes[size_t(bkgNode_)].rect);
            text = want;
        }
    }
    if (!dirty.isEmpty()) redraw(dirty);
}

// Exactly what a full redraw in renderDefaultScreen's order would give inside `region`: the clear,
// then every node that meets it, then the texts (drawn last, unclipped: outside the region they
// repaint pixels that already hold the same text, since any text change marks its old and new cells).
void ToasterScreen::redraw(const QRegion& region) {
    const QRegion r = region & QRect(0, 0, screen_.w, screen_.h);
    for (const QRect& rect : r) compose(rect);
    for (const auto& t : layout_.texts)
        if (!t.hiddenByDefault)
            vt::assets::drawText(screen_, font_, t.x, t.baseline ? t.y : t.y + font_.baseline, t.text, true, 0);
    for (const QRect& rect : r) scale(rect);
    const double dpr = devicePixelRatioF();
    for (const QRect& rect : r)
        update(QRectF(rect.x() * zoom_ / dpr, rect.y() * zoom_ / dpr, rect.width() * zoom_ / dpr,
                      rect.height() * zoom_ / dpr).toAlignedRect());
}

void ToasterScreen::compose(const QRect& clip) {
    const int w = screen_.w, margin = layout_.clearMarginX;
    for (int y = clip.top(); y <= clip.bottom(); ++y)  // blitter clear: colour 1 between 8-px colour-0 margins
        for (int x = clip.left(); x <= clip.right(); ++x)
            screen_.px[size_t(y) * size_t(w) + size_t(x)] = x < margin || x >= w - margin ? 0 : uint8_t(layout_.clearColour);
    for (int i : drawn_) {
        const auto& n = layout_.nodes[size_t(i)];
        const QRect part = clip & toQRect(n.rect);
        if (part.isEmpty()) continue;
        const auto& img = imageOf(i);
        for (int y = part.top(); y <= part.bottom(); ++y)
            std::memcpy(&screen_.px[size_t(y) * size_t(w) + size_t(part.left())],
                        &img.px[size_t(y - n.rect.y) * size_t(img.w) + size_t(part.left() - n.rect.x)],
                        size_t(part.width()));
    }
}

void ToasterScreen::scale(const QRect& rect) { replicate(screen_, rect, zoom_, colours_, scaled_); }

void ToasterScreen::rescaleAll() {
    scaled_ = QImage(screen_.w * zoom_, screen_.h * zoom_, QImage::Format_RGB32);
    scale(QRect(0, 0, screen_.w, screen_.h));
    update();
}

void ToasterScreen::paintEvent(QPaintEvent*) {
    QPainter p(this);
    // scaled_ is in device pixels: at the widget's own ratio Qt copies it 1:1, without scaling.
    scaled_.setDevicePixelRatio(devicePixelRatioF());
    p.drawImage(QPointF(0, 0), scaled_);
    const QSizeF shown = scaled_.deviceIndependentSize();
    const QColor border(colours_[0]);
    if (width() > shown.width()) p.fillRect(QRectF(shown.width(), 0, width() - shown.width(), height()), border);
    if (height() > shown.height()) p.fillRect(QRectF(0, shown.height(), shown.width(), height() - shown.height()), border);
}

QPointF ToasterScreen::toScreen(const QPointF& logical) const { return logical * devicePixelRatioF() / zoom_; }

// The top row of the T-bar is 0 (handle up), the bottom row 511.
int ToasterScreen::tbarFor(double screenY) const {
    const vt::Rect& r = layout_.tbarRect;
    return std::clamp(int(std::lround((std::floor(screenY) - r.y) * kTbarMax / (r.h - 1))), 0, kTbarMax);
}

void ToasterScreen::mousePressEvent(QMouseEvent* e) {
    if (e->button() != Qt::LeftButton) return QWidget::mousePressEvent(e);
    const QPointF pos = toScreen(e->position());
    const QPoint px(int(std::floor(pos.x())), int(std::floor(pos.y())));
    if (tbarNode_ >= 0 && toQRect(layout_.tbarRect).contains(px)) {
        dragTbar_ = tbarFor(pos.y());
        actions_.setTBar(dragTbar_);
        return;
    }
    for (const auto& c : controls_) {
        if (!toQRect(layout_.nodes[size_t(c.node)].hitRect).contains(px)) continue;
        switch (c.kind) {
        case Control::Bus: actions_.select(c.bus, c.source); break;
        case Control::Take: pressed_ = c.node; refresh(); actions_.take(); break;
        case Control::Auto: pressed_ = c.node; refresh(); actions_.autoTransition(vt::Direction::Forward); break;
        case Control::Speed: actions_.setSpeed(c.speed); break;
        case Control::Bkg: openBkgPopup(); break;
        case Control::Stub: pressed_ = c.node; refresh(); actions_.notImplemented(c.name); break;
        }
        return;
    }
}

void ToasterScreen::mouseMoveEvent(QMouseEvent* e) {
    if (dragTbar_ < 0) return QWidget::mouseMoveEvent(e);
    const int pos = tbarFor(toScreen(e->position()).y());
    if (pos == dragTbar_) return;
    dragTbar_ = pos;
    actions_.setTBar(pos);
}

void ToasterScreen::mouseReleaseEvent(QMouseEvent* e) {
    if (e->button() != Qt::LeftButton) return QWidget::mouseReleaseEvent(e);
    dragTbar_ = -1;
    pressed_ = -1;
    refresh();
}

// The list opens above the pop-up (it sits near the bottom of the screen), left edges aligned.
void ToasterScreen::openBkgPopup() {
    const vt::Rect& r = layout_.nodes[size_t(bkgNode_)].rect;
    const int top = std::max(0, r.y - bkgPopup_->art().h);
    const QPointF at = QPointF(r.x, top) * zoom_ / devicePixelRatioF();
    bkgPopup_->open(mapToGlobal(at.toPoint()), zoom_, colours_);
}

// Global Constraints: Return = Take, Space = Auto, Shift+Space = reverse Auto, , . / = S M F,
// F1-F7 = Main, 1-7 = Preview, q-u = Superimpose.
void ToasterScreen::keyPressEvent(QKeyEvent* e) {
    static constexpr Qt::Key kOverlay[] = {Qt::Key_Q, Qt::Key_W, Qt::Key_E, Qt::Key_R, Qt::Key_T, Qt::Key_Y, Qt::Key_U};
    const int k = e->key();
    auto handled = [&] {
        e->accept();
        return !e->isAutoRepeat();
    };
    if (k == Qt::Key_Return || k == Qt::Key_Enter) {
        if (handled()) actions_.take();
    } else if (k == Qt::Key_Space) {
        if (handled())
            actions_.autoTransition(e->modifiers() & Qt::ShiftModifier ? vt::Direction::Reverse : vt::Direction::Forward);
    } else if (k == Qt::Key_Comma || k == Qt::Key_Period || k == Qt::Key_Slash) {
        if (handled())
            actions_.setSpeed(k == Qt::Key_Comma ? vt::Speed::Slow : k == Qt::Key_Period ? vt::Speed::Medium : vt::Speed::Fast);
    } else if (k >= Qt::Key_F1 && k <= Qt::Key_F7) {
        if (handled()) actions_.select(vt::Bus::Main, vt::Source(k - Qt::Key_F1));
    } else if (k >= Qt::Key_1 && k <= Qt::Key_7) {
        if (handled()) actions_.select(vt::Bus::Preview, vt::Source(k - Qt::Key_1));
    } else if (const auto* o = std::find(std::begin(kOverlay), std::end(kOverlay), k); o != std::end(kOverlay)) {
        if (handled()) actions_.select(vt::Bus::Overlay, vt::Source(o - std::begin(kOverlay)));
    } else {
        QWidget::keyPressEvent(e);
    }
}
