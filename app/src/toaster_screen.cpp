#include "toaster_screen.h"

#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QToolTip>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <stdexcept>

#include "pixel_replicate.h"

namespace {

constexpr int kTbarMax = 511;
constexpr int kTbarImages = kTbarMax / 6 + 1;  // 86

QRect toQRect(const vt::Rect& r) { return {r.x, r.y, r.w, r.h}; }

QList<QRgb> colourTable(const vt::assets::Palette& p) {
    QList<QRgb> t;
    for (const auto& c : p) t << qRgb(c.r, c.g, c.b);
    return t;
}

// Bus rows: node id prefix, and the button suffixes in Source order (In1..In4, DV1, DV2, BKG).
constexpr const char* kSourceSuffix[] = {"1", "2", "3", "4", "dv1", "dv2", "bkg"};
struct BusRow { const char* prefix; vt::Bus bus; };
constexpr BusRow kBusRows[] = {{"main_", vt::Bus::Main}, {"prev_", vt::Bus::Preview}, {"sup_", vt::Bus::Superimpose}};

// Drawn and pressable, but nothing behind them in v1.
struct Stub { const char* id; const char* name; };
constexpr Stub kStubs[] = {
    {"fs_stabilize", "STABILIZE"},
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
    replicate(QRect(0, 0, art_.w, art_.h), zoom,
              [&](int x, int y) { return colours[art_.px[size_t(y) * size_t(art_.w) + size_t(x)]]; }, scaled_);
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
    control("fs_freeze", {Control::Freeze, -1});
    control("fs_live", {Control::Live, -1});
    for (int i = 0; i < 4; ++i) control(("fs_" + std::to_string(i + 1)).c_str(), {Control::Grab, -1, {}, {}, {}, i});
    control("fs_save", {Control::Save, -1});
    control("key_off", {Control::Key, -1, {}, {}, {}, 0, {}, vt::KeyMode::Off});
    control("key_black", {Control::Key, -1, {}, {}, {}, 0, {}, vt::KeyMode::Black});
    control("key_white", {Control::Key, -1, {}, {}, {}, 0, {}, vt::KeyMode::White});
    control("key_level_popup", {Control::KeyLevel, -1});
    for (const auto& s : kStubs) control(s.id, {Control::Stub, -1, {}, {}, {}, 0, QString::fromLatin1(s.name)});

    tbarNode_ = nodeIndex("tbar");
    if (!visible(tbarNode_)) tbarNode_ = -1;
    bkgNode_ = nodeIndex("bkg_popup");
    for (size_t i = 0; i < layout_.texts.size(); ++i)
        if (layout_.texts[i].id == "bkg_popup_text") bkgText_ = int(i);
        else if (layout_.texts[i].id == "key_level_text") levelText_ = int(i);
    levelNode_ = nodeIndex("key_level_popup");

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
    if (levelText_ >= 0) layout_.texts[size_t(levelText_)].text = levelText();
    redraw(QRect(0, 0, layout_.screenW, layout_.screenH));
}

int ToasterScreen::nodeIndex(const char* id) const {
    for (size_t i = 0; i < layout_.nodes.size(); ++i)
        if (layout_.nodes[i].id == id) return int(i);
    return -1;
}

void ToasterScreen::setZoom(int z) {
    z = std::max(1, z);
    if (z == preferredZoom_) return;
    preferredZoom_ = z;
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

QImage ToasterScreen::renderRgbForTest() const {
    QImage img(screen_.w, screen_.h, QImage::Format_RGB32);
    replicate(QRect(0, 0, screen_.w, screen_.h), 1, [this](int x, int y) { return rgbAt(x, y); }, img);
    return img;
}

void ToasterScreen::setCroutons(std::vector<vt::assets::Crouton> croutons) {
    project_.setCroutons(std::move(croutons));
    projectChanged(true);
}

void ToasterScreen::setScrollRow(int row) { projectChanged(project_.setScrollRow(row)); }
void ToasterScreen::setLoaded(int index) { projectChanged(project_.setLoaded(index)); }
void ToasterScreen::setNext(int index) { projectChanged(project_.setNext(index)); }

void ToasterScreen::projectChanged(bool changed) {
    if (!changed) return;
    scale(ProjectGrid::area());
    update(logicalRect(ProjectGrid::area()));
}

QSize ToasterScreen::sizeAt(int zoom) const {
    const double dpr = devicePixelRatioF();
    return {int(std::ceil(layout_.screenW * zoom / dpr)), int(std::ceil(layout_.screenH * zoom / dpr))};
}

QSize ToasterScreen::sizeHint() const { return sizeAt(preferredZoom_); }
QSize ToasterScreen::minimumSizeHint() const { return sizeAt(1); }

// The largest whole zoom whose 768x481 x zoom device pixels fit the widget, centred at a whole
// device-pixel offset (top left when even zoom 1 does not fit). Returns whether either changed.
bool ToasterScreen::snap() {
    const double dpr = devicePixelRatioF();
    const int w = int(std::floor(width() * dpr + 1e-6)), h = int(std::floor(height() * dpr + 1e-6));
    const int z = std::max(1, std::min(w / screen_.w, h / screen_.h));
    const QPoint offset(std::max(0, (w - screen_.w * z) / 2), std::max(0, (h - screen_.h * z) / 2));
    if (z == zoom_ && offset == offset_) return false;
    const bool rezoom = z != zoom_;
    zoom_ = z;
    offset_ = offset;
    if (rezoom) rescaleAll();
    else update();
    return true;
}

int ToasterScreen::lookOf(int node) const {
    if (node == tbarNode_)  // the handler 0x5e000: state = position / 6; always the engine's (Ruling 8)
        return std::clamp(state_.tbar, 0, kTbarMax) / 6;
    if (node == pressed_) return 1;
    for (const auto& c : controls_)
        if (c.node == node) {
            if (c.kind == Control::Bus) {
                const vt::Source on = c.bus == vt::Bus::Main ? state_.main
                                      : c.bus == vt::Bus::Preview ? state_.preview : state_.superimpose;
                return c.source == on;
            }
            if (c.kind == Control::Speed) return c.speed == state_.speed;  // Variable: none of S/M/F
            if (c.kind == Control::Freeze) return state_.frozen;   // grab and SAVE are momentary
            if (c.kind == Control::Live) return !state_.frozen;
            if (c.kind == Control::Key) return c.keyMode == state_.keyMode;
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

// C8: three zero-padded digits.
std::string ToasterScreen::levelText() const {
    char buf[16];
    std::snprintf(buf, sizeof buf, "%03d", std::clamp(state_.keyLevel, 0, vt::kKeyLevelMax));
    return buf;
}

QRect ToasterScreen::textRect(int text, const std::string& text_) const {
    const auto& t = layout_.texts[size_t(text)];
    const int top = t.baseline ? t.y - font_.baseline : t.y;
    return {t.x, top, vt::assets::textWidth(font_, text_), font_.ySize};
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
            dirty += textRect(bkgText_, text);
            dirty += textRect(bkgText_, want);
            if (bkgNode_ >= 0) dirty += toQRect(layout_.nodes[size_t(bkgNode_)].rect);
            text = want;
        }
    }
    if (levelText_ >= 0) {
        auto& text = layout_.texts[size_t(levelText_)].text;
        if (const std::string want = levelText(); want != text) {
            dirty += textRect(levelText_, text);
            dirty += textRect(levelText_, want);
            if (levelNode_ >= 0) dirty += toQRect(layout_.nodes[size_t(levelNode_)].rect);
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
    for (const QRect& rect : r) update(logicalRect(rect));
}

QRect ToasterScreen::logicalRect(const QRect& r) const {
    const double dpr = devicePixelRatioF();
    return QRectF((r.x() * zoom_ + offset_.x()) / dpr, (r.y() * zoom_ + offset_.y()) / dpr, r.width() * zoom_ / dpr,
                  r.height() * zoom_ / dpr).toAlignedRect();
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

QRgb ToasterScreen::rgbAt(int x, int y) const {
    const QRect a = ProjectGrid::area();
    if (a.contains(x, y)) {
        const QRgb c = reinterpret_cast<const QRgb*>(project_.image().constScanLine(y - a.y()))[x - a.x()];
        if (qAlpha(c)) return c;
    }
    return colours_[screen_.px[size_t(y) * size_t(screen_.w) + size_t(x)]];
}

void ToasterScreen::scale(const QRect& rect) {
    replicate(rect, zoom_, [this](int x, int y) { return rgbAt(x, y); }, scaled_);
}

void ToasterScreen::rescaleAll() {
    scaled_ = QImage(screen_.w * zoom_, screen_.h * zoom_, QImage::Format_RGB32);
    scale(QRect(0, 0, screen_.w, screen_.h));
    update();
}

void ToasterScreen::resizeEvent(QResizeEvent*) {
    if (snap() && bkgPopup_ && bkgPopup_->isVisible()) openBkgPopup();
}

void ToasterScreen::paintEvent(QPaintEvent*) {
    QPainter p(this);
    // scaled_ is in device pixels: at the widget's own ratio Qt copies it 1:1, without scaling, to
    // the whole device-pixel offset_. The margins around it (filled first: the panel covers any
    // pixel they share) are the panel clear colour.
    const double dpr = devicePixelRatioF();
    scaled_.setDevicePixelRatio(dpr);
    const QRectF panel(QPointF(offset_) / dpr, scaled_.deviceIndependentSize());
    const QColor clear(colours_[layout_.clearColour]);
    const qreal w = width(), h = height();
    for (const QRectF& m : {QRectF(0, 0, w, panel.top()), QRectF(0, panel.bottom(), w, h - panel.bottom()),
                            QRectF(0, panel.top(), panel.left(), panel.height()),
                            QRectF(panel.right(), panel.top(), w - panel.right(), panel.height())})
        if (!m.isEmpty()) p.fillRect(m, clear);
    p.drawImage(panel.topLeft(), scaled_);
}

QPointF ToasterScreen::toScreen(const QPointF& logical) const {
    return (logical * devicePixelRatioF() - QPointF(offset_)) / zoom_;
}

QPoint ToasterScreen::toScreenPixel(const QPointF& logical) const {
    const QPointF pos = toScreen(logical);
    return {int(std::floor(pos.x())), int(std::floor(pos.y()))};
}

// The top row of the T-bar is 0 (handle up), the bottom row 511.
int ToasterScreen::tbarFor(double screenY) const {
    const vt::Rect& r = layout_.tbarRect;
    return std::clamp(int(std::lround((std::floor(screenY) - r.y) * kTbarMax / (r.h - 1))), 0, kTbarMax);
}

void ToasterScreen::startRightDrag(double screenY) {
    rightDrag_ = true;
    dragTbar_ = tbarAtPress_ = state_.tbar;
    pressY_ = screenY;
}

// BackToTBar keeps the position into the drag that follows; outside positioning it is ignored.
void ToasterScreen::grabTbar() {
    if (state_.positioning) actions_.endPositioning(vt::PositionEnd::BackToTBar);
}

void ToasterScreen::endDrag() {
    dragTbar_ = -1;
    rightDrag_ = false;
    actions_.releaseTBar();
}

void ToasterScreen::mousePressEvent(QMouseEvent* e) {
    if (e->button() == Qt::RightButton && dragTbar_ < 0) {
        grabTbar();
        return startRightDrag(toScreen(e->position()).y());
    }
    if (e->button() != Qt::LeftButton) return QWidget::mousePressEvent(e);
    const QPointF pos = toScreen(e->position());
    const QPoint px = toScreenPixel(e->position());
    if (ProjectGrid::area().contains(px)) {
        if (const int i = project_.indexAt(px); i >= 0) {
            setNext(i);
            actions_.loadCrouton(i, false);
        }
        return;
    }
    if (tbarNode_ >= 0 && toQRect(layout_.tbarRect).contains(px)) {
        grabTbar();
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
        case Control::Freeze: actions_.setFrozen(true); break;
        case Control::Live: actions_.setFrozen(false); break;
        case Control::Grab: pressed_ = c.node; refresh(); actions_.grab(c.input); break;
        case Control::Save: pressed_ = c.node; refresh(); actions_.saveFramestore(); break;
        case Control::Key: actions_.setKeyMode(c.keyMode); break;
        case Control::KeyLevel: dragLevel_ = levelAtPress_ = state_.keyLevel; pressX_ = pos.x(); break;
        case Control::Stub: pressed_ = c.node; refresh(); actions_.notImplemented(c.name); break;
        }
        return;
    }
}

void ToasterScreen::mouseMoveEvent(QMouseEvent* e) {
    if (rightDrag_) {  // the T-bar's own scale, from where the drag began
        const int pos = std::clamp(tbarAtPress_ + int(std::lround((toScreen(e->position()).y() - pressY_) * kTbarMax /
                                                                   (layout_.tbarRect.h - 1))),
                                   0, kTbarMax);
        if (pos != dragTbar_) actions_.setTBar(dragTbar_ = pos);
        return;
    }
    if (dragLevel_ >= 0) {  // C8: L moves by half the drag, right raises it
        const int level = std::clamp(levelAtPress_ + int((toScreen(e->position()).x() - pressX_) / 2), 0, vt::kKeyLevelMax);
        if (level != dragLevel_) actions_.setKeyLevel(dragLevel_ = level);
        return;
    }
    if (dragTbar_ < 0) return QWidget::mouseMoveEvent(e);
    const int pos = tbarFor(toScreen(e->position()).y());
    if (pos == dragTbar_) return;
    dragTbar_ = pos;
    actions_.setTBar(pos);
}

void ToasterScreen::mouseReleaseEvent(QMouseEvent* e) {
    if (e->button() == Qt::RightButton && rightDrag_) return endDrag();
    if (e->button() != Qt::LeftButton) return QWidget::mouseReleaseEvent(e);
    if (dragTbar_ >= 0 && !rightDrag_) endDrag();
    dragLevel_ = -1;
    pressed_ = -1;
    refresh();
}

// Qt sends press, release, double click, release: the press has loaded the crouton already.
void ToasterScreen::mouseDoubleClickEvent(QMouseEvent* e) {
    const QPoint px = toScreenPixel(e->position());
    if (e->button() != Qt::LeftButton || !ProjectGrid::area().contains(px)) return QWidget::mouseDoubleClickEvent(e);
    if (const int i = project_.indexAt(px); i >= 0) actions_.loadCrouton(i, true);
}

// One notch (120 eighths of a degree) over the grid scrolls one row; wheel down shows later rows.
void ToasterScreen::wheelEvent(QWheelEvent* e) {
    if (!ProjectGrid::area().contains(toScreenPixel(e->position()))) return QWidget::wheelEvent(e);
    wheelAngle_ += e->angleDelta().y();
    const int rows = wheelAngle_ / 120;
    wheelAngle_ -= rows * 120;
    if (rows) setScrollRow(project_.scrollRow() - rows);
    e->accept();
}

bool ToasterScreen::event(QEvent* e) {
    if ((e->type() == QEvent::WindowDeactivate || e->type() == QEvent::Hide) && dragTbar_ >= 0) endDrag();
    if (e->type() == QEvent::DevicePixelRatioChange) {
        updateGeometry();
        snap();
        update();
        if (bkgPopup_ && bkgPopup_->isVisible()) openBkgPopup();
    }
    if (e->type() != QEvent::ToolTip) return QWidget::event(e);
    const auto* help = static_cast<QHelpEvent*>(e);
    if (const int i = project_.indexAt(toScreenPixel(help->pos())); i >= 0) {
        QToolTip::showText(help->globalPos(), QString::fromStdString(project_.croutons()[size_t(i)].name), this);
    } else {
        QToolTip::hideText();
        e->ignore();
    }
    return true;
}

// The list opens above the pop-up (it sits near the bottom of the screen), left edges aligned.
void ToasterScreen::openBkgPopup() {
    const vt::Rect& r = layout_.nodes[size_t(bkgNode_)].rect;
    const int top = std::max(0, r.y - bkgPopup_->art().h);
    const QPointF at = (QPointF(r.x, top) * zoom_ + QPointF(offset_)) / devicePixelRatioF();
    bkgPopup_->open(mapToGlobal(at.toPoint()), zoom_, colours_);
}

// Global Constraints: Return = Take, Space = Auto, Shift+Space = reverse Auto (while positioning, Space
// and Return keep the position instead), , . / = S M F,
// F1-F7 = Main, 1-7 = Preview, q-u = Superimpose, f = FREEZE, l = LIVE, o [ ] = key Off / Black / White.
bool ToasterScreen::switcherKey(QKeyEvent* e) {
    static constexpr Qt::Key kOverlay[] = {Qt::Key_Q, Qt::Key_W, Qt::Key_E, Qt::Key_R, Qt::Key_T, Qt::Key_Y, Qt::Key_U};
    const int k = e->key();
    auto handled = [&] {
        e->accept();
        return !e->isAutoRepeat();
    };
    if (state_.positioning && (k == Qt::Key_Return || k == Qt::Key_Enter || k == Qt::Key_Space)) {  // 0xb71dc
        if (handled()) actions_.endPositioning(vt::PositionEnd::KeepAndStop);
    } else if (k == Qt::Key_Return || k == Qt::Key_Enter) {
        if (handled()) actions_.take();
    } else if (k == Qt::Key_Space) {
        if (handled())
            actions_.autoTransition(e->modifiers() & Qt::ShiftModifier ? vt::Direction::Reverse : vt::Direction::Forward);
    } else if (k == Qt::Key_Comma || k == Qt::Key_Period || k == Qt::Key_Slash) {
        if (handled())
            actions_.setSpeed(k == Qt::Key_Comma ? vt::Speed::Slow : k == Qt::Key_Period ? vt::Speed::Medium : vt::Speed::Fast);
    } else if (k == Qt::Key_F || k == Qt::Key_L) {
        if (handled()) actions_.setFrozen(k == Qt::Key_F);
    } else if (k == Qt::Key_O || k == Qt::Key_BracketLeft || k == Qt::Key_BracketRight) {
        if (handled())
            actions_.setKeyMode(k == Qt::Key_O ? vt::KeyMode::Off : k == Qt::Key_BracketLeft ? vt::KeyMode::Black : vt::KeyMode::White);
    } else if (k >= Qt::Key_F1 && k <= Qt::Key_F7) {
        if (handled()) actions_.select(vt::Bus::Main, vt::Source(k - Qt::Key_F1));
    } else if (k >= Qt::Key_1 && k <= Qt::Key_7) {
        if (handled()) actions_.select(vt::Bus::Preview, vt::Source(k - Qt::Key_1));
    } else if (const auto* o = std::find(std::begin(kOverlay), std::end(kOverlay), k); o != std::end(kOverlay)) {
        if (handled()) actions_.select(vt::Bus::Superimpose, vt::Source(o - std::begin(kOverlay)));
    } else {
        return false;
    }
    return true;
}

void ToasterScreen::keyPressEvent(QKeyEvent* e) {
    if (!switcherKey(e)) QWidget::keyPressEvent(e);
}
