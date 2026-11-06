#include "ScreenFit.h"

#include "../Common.h"

#include <QGuiApplication>
#include <QScreen>

namespace kn {

namespace {
// 与屏幕可用区的最小重叠比例，低于该比例视为已越界
const qreal kMinVisibleRatio = 0.30;
}

bool ScreenFit::screensReady()
{
    const QList<QScreen *> screens = QGuiApplication::screens();
    if (screens.isEmpty()) {
        return false;
    }
    QScreen *primary = QGuiApplication::primaryScreen();
    if (!primary) {
        return false;
    }
    const QRect avail = primary->availableGeometry();
    return avail.width() > 0 && avail.height() > 0;
}

static QRect clampInto(const QRect &win, const QRect &avail)
{
    int w = qMin(win.width(), avail.width());
    int h = qMin(win.height(), avail.height());
    w = qMax<int>(w, MinWidth);
    h = qMax<int>(h, MinHeight);
    // 极端窄屏兜底
    w = qMin(w, avail.width());
    h = qMin(h, avail.height());

    int x = win.x();
    int y = win.y();
    if (x + w > avail.right() + 1) {
        x = avail.right() + 1 - w;
    }
    if (x < avail.x()) {
        x = avail.x();
    }
    if (y + h > avail.bottom() + 1) {
        y = avail.bottom() + 1 - h;
    }
    if (y < avail.y()) {
        y = avail.y();
    }
    return QRect(x, y, w, h);
}

QRect ScreenFit::fit(const QRect &geometry)
{
    const QList<QScreen *> screens = QGuiApplication::screens();
    if (screens.isEmpty() || !geometry.isValid() || geometry.width() <= 0 || geometry.height() <= 0) {
        return defaultGeometry(0);
    }

    const qint64 area = qint64(geometry.width()) * qint64(geometry.height());
    QScreen *best = nullptr;
    qint64 bestOverlap = 0;

    for (QScreen *screen : screens) {
        const QRect avail = screen->availableGeometry();
        const QRect inter = avail.intersected(geometry);
        if (inter.isEmpty()) {
            continue;
        }
        const qint64 overlap = qint64(inter.width()) * qint64(inter.height());
        if (overlap > bestOverlap) {
            bestOverlap = overlap;
            best = screen;
        }
    }

    // 中心点落在某屏内时优先采用该屏，避免"跨屏各露一半"被判为有效
    const QPoint center = geometry.center();
    for (QScreen *screen : screens) {
        if (screen->availableGeometry().contains(center)) {
            best = screen;
            bestOverlap = area;
            break;
        }
    }

    if (best && area > 0 && (qreal(bestOverlap) / qreal(area)) >= kMinVisibleRatio) {
        return clampInto(geometry, best->availableGeometry());
    }

    // 完全越界：回到主屏可用区
    QScreen *primary = QGuiApplication::primaryScreen();
    if (!primary) {
        primary = screens.first();
    }
    return clampInto(QRect(geometry.topLeft(), QSize(DefaultWidth, DefaultHeight)),
                     primary->availableGeometry());
}

QRect ScreenFit::defaultGeometry(int cascadeIndex)
{
    QScreen *primary = QGuiApplication::primaryScreen();
    if (!primary) {
        const QList<QScreen *> screens = QGuiApplication::screens();
        if (screens.isEmpty()) {
            return QRect(100, 100, DefaultWidth, DefaultHeight);
        }
        primary = screens.first();
    }

    const QRect avail = primary->availableGeometry();
    const int step = 28;
    const int span = 12;   // 层叠 12 次后回到起点
    const int offset = (cascadeIndex % span) * step;

    int x = avail.x() + 120 + offset;
    int y = avail.y() + 100 + offset;

    QRect geo(x, y, DefaultWidth, DefaultHeight);
    if (x + geo.width() > avail.right() + 1 || y + geo.height() > avail.bottom() + 1) {
        geo.moveTo(avail.x() + 60, avail.y() + 60);
    }
    return clampInto(geo, avail);
}

} // namespace kn
