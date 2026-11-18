#include "WorkspaceStyle.h"

#include <QStyleOption>

namespace kl {

WorkspaceStyle::WorkspaceStyle() : QProxyStyle(QStringLiteral("Fusion")) {}

void WorkspaceStyle::drawPrimitive(PrimitiveElement element, const QStyleOption* option,
                                  QPainter* painter, const QWidget* widget) const {
    if (element == PE_FrameFocusRect) return;
    QProxyStyle::drawPrimitive(element, option, painter, widget);
}

} // namespace kl
