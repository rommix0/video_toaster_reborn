#pragma once
// The one place a picture is zoomed: exact pixel replication, no filtering. Shared by the Switcher
// screen, the BKG list and the Program/Preview monitors.
#include <QImage>
#include <QRect>
#include <QRgb>
#include <algorithm>
#include <cstring>
#include <vector>

// Exact pixel replication of rect (Toaster pixels) into dst: each Toaster pixel (x, y) becomes
// zoom x zoom device pixels of colourAt(x, y); the rect lands at (rect.x * zoom, rect.y * zoom) in dst.
// The screen with its Project grid, the BKG list and the monitors all go through here. Any future
// pixel-aspect option belongs here (and in the Toaster <-> logical mappings).
template <class ColourAt>
inline void replicate(const QRect& rect, int zoom, const ColourAt& colourAt, QImage& dst) {
    std::vector<QRgb> row(size_t(rect.width()) * size_t(zoom));
    for (int y = rect.top(); y <= rect.bottom(); ++y) {
        for (int x = 0; x < rect.width(); ++x) std::fill_n(row.begin() + x * zoom, zoom, colourAt(rect.left() + x, y));
        for (int dy = 0; dy < zoom; ++dy)
            std::memcpy(reinterpret_cast<QRgb*>(dst.scanLine(y * zoom + dy)) + rect.left() * zoom, row.data(),
                        row.size() * sizeof(QRgb));
    }
}
