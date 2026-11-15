#include "ListAppearance.h"

#include <QPainter>
#include <QPixmap>
#include "AppIcons.h"
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
    QFont font = ds::qfont(ds::FontScene::Body);
    font.setPixelSize(qMax(8, qRound(rect.height())));
    painter->setFont(font);
    painter->drawText(rect, Qt::AlignCenter, name);
    painter->restore();
}

}  // namespace ListAppearance
}  // namespace kl
