#include "ListAppearance.h"

#include <QPainter>
#include <QPixmap>
#include <QtMath>
#include "AppIcons.h"
#include "design/ThemeManager.h"
#include "design/TypeSystem.h"

namespace kl {
namespace ListAppearance {

QString iconName(const TaskList& list) {
    if (!list.icon.isEmpty()) return list.icon;
    if (list.inbox) return QStringLiteral("inbox-list");
    return list.kind == EntityKind::Note ? QStringLiteral("note-list")
                                         : QStringLiteral("normal-list");
}

QColor color(const TaskList& list) {
    const QColor value(list.color);
    return value.isValid() ? value : QColor();
}

QIcon icon(const TaskList& list, int size, const QColor& fallbackColor) {
    const QString name = iconName(list);
    if (AppIcons::hasIcon(name)) {
        const QColor listColor = color(list);
        return AppIcons::icon(name, listColor.isValid() ? listColor : fallbackColor, size);
    }
    QPixmap pixmap(size * 2, size * 2);
    pixmap.setDevicePixelRatio(2.0);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    paint(&painter, list, QRectF(0, 0, size, size), fallbackColor);
    return QIcon(pixmap);
}

void paint(QPainter* painter, const TaskList& list, const QRectF& rect,
           const QColor& fallbackColor) {
    if (!painter) return;
    const QString name = iconName(list);
    if (AppIcons::hasIcon(name)) {
        const QColor listColor = color(list);
        AppIcons::paint(painter, name, rect,
                        listColor.isValid() ? listColor : fallbackColor);
        return;
    }
    painter->save();
    const QRectF content = rect.adjusted(1.0, 1.0, -1.0, -1.0);
    if (content.width() <= 0.0 || content.height() <= 0.0 || name.isEmpty()) {
        painter->restore();
        return;
    }
    QFont font = ds::emojiFont();
    int pixelSize = qBound(8, qFloor(content.height()), 19);
    font.setPixelSize(pixelSize);
    QFontMetricsF metrics(font);
    QRectF bounds = metrics.boundingRect(name);
    while (pixelSize > 8
           && (bounds.width() > content.width() || bounds.height() > content.height())) {
        font.setPixelSize(--pixelSize);
        metrics = QFontMetricsF(font);
        bounds = metrics.boundingRect(name);
    }
    painter->setFont(font);
    const QColor textColor = fallbackColor.isValid()
        ? fallbackColor : ThemeManager::instance().color(ds::ColorToken::TextSecondary);
    // 背景绘制常把画笔设为 NoPen；文本必须显式恢复，不能依赖调用方状态。
    painter->setPen(textColor);
    painter->drawText(content, Qt::AlignCenter, name);
    painter->restore();
}

}  // namespace ListAppearance
}  // namespace kl
