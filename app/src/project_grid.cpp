#include "project_grid.h"

#include <QComboBox>
#include <QScreen>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>

#include "toaster_screen.h"

namespace {

namespace fs = std::filesystem;
using vt::assets::kCroutonH;
using vt::assets::kCroutonW;

constexpr int kScreenW = 768, kScreenH = 481;

// Highlights (my call, documented as a guess: the original drawing was not found, SCREEN.md §6.2),
// Amiga 12-bit colours expanded x17.
const QRgb kLoadedColour = qRgb(0xff, 0xee, 0x77);  // FE7: a 2-px frame just inside the cell
const QRgb kNextColour = qRgb(0xdd, 0xdd, 0xdd);    // DDD: 2-px corner brackets, drawn over the frame
constexpr int kLine = 2, kArm = 10;

std::string lower(std::string s) {
    for (auto& c : s) c = char(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
    return s;
}

std::string utf8(const fs::path& p) {
    const auto u8 = p.generic_u8string();
    return {u8.begin(), u8.end()};
}

QString toQString(const fs::path& p) { return QString::fromStdU16String(p.generic_u16string()); }

}  // namespace

int fitZoom(QSize available, int chromeHeight) {
    return std::max(1, std::min(available.width() / kScreenW, (available.height() - chromeHeight) / kScreenH));
}

int defaultZoom(const QScreen& screen, int chromeHeight) {
    const double dpr = screen.devicePixelRatio();
    const QSize avail = screen.availableGeometry().size();
    return fitZoom(QSize(int(avail.width() * dpr), int(avail.height() * dpr)), int(std::ceil(chromeHeight * dpr)));
}

ProjectGrid::ProjectGrid() { compose(); }

void ProjectGrid::setCroutons(std::vector<vt::assets::Crouton> croutons) {
    croutons_ = std::move(croutons);
    colours_.clear();
    for (const auto& c : croutons_) {
        QList<QRgb> t(256, qRgb(0, 0, 0));
        for (size_t i = 0; i < std::min<size_t>(c.palette.size(), 256); ++i)
            t[qsizetype(i)] = qRgb(c.palette[i].r, c.palette[i].g, c.palette[i].b);
        colours_.push_back(std::move(t));
    }
    scrollRow_ = 0;
    loaded_ = next_ = -1;
    compose();
}

int ProjectGrid::rowCount() const { return int((croutons_.size() + kColumns - 1) / kColumns); }

bool ProjectGrid::setScrollRow(int row) {
    row = std::clamp(row, 0, std::max(0, rowCount() - kRows));
    if (row == scrollRow_) return false;
    scrollRow_ = row;
    compose();
    return true;
}

bool ProjectGrid::setLoaded(int index) {
    if (index < 0 || index >= int(croutons_.size())) index = -1;
    if (index == loaded_) return false;
    loaded_ = index;
    compose();
    return true;
}

bool ProjectGrid::setNext(int index) {
    if (index < 0 || index >= int(croutons_.size())) index = -1;
    if (index == next_) return false;
    next_ = index;
    compose();
    return true;
}

int ProjectGrid::indexAt(QPoint px) const {
    if (!area().contains(px)) return -1;
    const int col = (px.x() - kLeft) / kCroutonW, row = (px.y() - kTop) / kCroutonH;
    const int i = (scrollRow_ + row) * kColumns + col;
    return i < int(croutons_.size()) ? i : -1;
}

void ProjectGrid::compose() {
    image_ = QImage(area().size(), QImage::Format_ARGB32);
    image_.fill(Qt::transparent);
    for (int slot = 0; slot < kColumns * kRows; ++slot) {
        const int i = scrollRow_ * kColumns + slot;
        if (i >= int(croutons_.size())) break;
        const auto& img = croutons_[size_t(i)].image;
        if (img.w != kCroutonW || img.h != kCroutonH || img.px.size() != size_t(kCroutonW) * kCroutonH) continue;
        const QList<QRgb>& colours = colours_[size_t(i)];
        const int x0 = slot % kColumns * kCroutonW, y0 = slot / kColumns * kCroutonH;
        auto set = [&](int x, int y, QRgb c) { reinterpret_cast<QRgb*>(image_.scanLine(y0 + y))[x0 + x] = c; };
        for (int y = 0; y < kCroutonH; ++y)
            for (int x = 0; x < kCroutonW; ++x) set(x, y, colours[img.px[size_t(y) * size_t(img.w) + size_t(x)]]);
        if (i == loaded_)
            for (int y = 0; y < kCroutonH; ++y)
                for (int x = 0; x < kCroutonW; ++x)
                    if (x < kLine || y < kLine || x >= kCroutonW - kLine || y >= kCroutonH - kLine) set(x, y, kLoadedColour);
        if (i == next_)
            for (int y = 0; y < kCroutonH; ++y)
                for (int x = 0; x < kCroutonW; ++x) {
                    const bool nearX = x < kArm || x >= kCroutonW - kArm, nearY = y < kArm || y >= kCroutonH - kArm;
                    const bool edgeX = x < kLine || x >= kCroutonW - kLine, edgeY = y < kLine || y >= kCroutonH - kLine;
                    if ((edgeY && nearX) || (edgeX && nearY)) set(x, y, kNextColour);
                }
    }
}

ProjectView::ProjectView(const fs::path& effectsRoot, ToasterScreen* screen, QWidget* parent)
    : QWidget(parent), picker_(new QComboBox(this)), screen_(screen) {
    auto* box = new QVBoxLayout(this);
    box->setContentsMargins(0, 0, 0, 0);
    box->addWidget(picker_, 0, Qt::AlignLeft);
    box->addWidget(screen_, 1, Qt::AlignLeft | Qt::AlignTop);
    const fs::path root = effectsRoot.lexically_normal();
    for (const auto& d : croutonDrawers(root))
        picker_->addItem(toQString(d.lexically_relative(root)), toQString(d));
    connect(picker_, &QComboBox::currentIndexChanged, this, &ProjectView::showDrawer);
    if (picker_->count() > 0) showDrawer(0);
}

void ProjectView::showDrawer(int index) {
    if (index < 0) return screen_->setCroutons({});
    screen_->setCroutons(vt::assets::scanCroutons(fs::path(picker_->itemData(index).toString().toStdU16String())));
}

std::vector<fs::path> croutonDrawers(const fs::path& effectsRoot) {
    std::vector<fs::path> out;
    std::error_code ec;
    auto holdsCroutons = [](const fs::path& dir) {
        std::vector<std::string> names;
        std::error_code e;
        for (const auto& f : fs::directory_iterator(dir, e))
            if (std::error_code fe; f.is_regular_file(fe)) names.push_back(utf8(f.path().filename()));
        return std::any_of(names.begin(), names.end(), [&](const std::string& n) {
            return std::find(names.begin(), names.end(), n + ".i") != names.end() ||
                   std::find(names.begin(), names.end(), n + ".I") != names.end();
        });
    };
    for (auto it = fs::recursive_directory_iterator(effectsRoot, ec); !ec && it != fs::recursive_directory_iterator();
         it.increment(ec))
        if (std::error_code de; it->is_directory(de) && holdsCroutons(it->path())) out.push_back(it->path());
    std::sort(out.begin(), out.end(), [](const fs::path& a, const fs::path& b) {
        const std::string sa = utf8(a), sb = utf8(b), la = lower(sa), lb = lower(sb);
        return la != lb ? la < lb : sa < sb;
    });
    return out;
}
