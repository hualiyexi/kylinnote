#include "WindowBorderOverlay.h"

#include <QPainter>
#include <QPainterPath>
#include "design/ThemeManager.h"

namespace kl {

WindowBorderOverlay::WindowBorderOverlay(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("WindowBorderOverlay"));
    setAttribute(Qt::WA_TransparentForMouseEvents, true);
    setAttribute(Qt::WA_TranslucentBackground, true);
    setAttribute(Qt::WA_StyledBackground, false);
}

void WindowBorderOverlay::setRounded(bool rounded) {
    m_rounded = rounded;
    update();
}

void WindowBorderOverlay::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    const QRectF bounds = QRectF(rect()).adjusted(0.6, 0.6, -0.6, -0.6);
    QPainterPath path;
    if (m_rounded) path.addRoundedRect(bounds, 10.0, 10.0);
    else path.addRect(bounds);
    painter.setPen(QPen(ThemeManager::instance().color(ds::ColorToken::WindowBorder), 1.2));
    painter.setBrush(Qt::NoBrush);
    painter.drawPath(path);
}

}  // namespace kl
