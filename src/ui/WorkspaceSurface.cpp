#include "WorkspaceSurface.h"

#include <QPainter>
#include <QPainterPath>
#include <QRadialGradient>
#include <QResizeEvent>
#include "design/ThemeManager.h"

namespace kl {
namespace {
constexpr int kShadowMargin = 16;
constexpr qreal kCornerRadius = 10.0;

void drawCornerShadow(QPainter& painter, const QPointF& center, int quadrant) {
    // 径向渐变只保留窗外象限，避免多层描边形成可见阶梯。
    QRadialGradient gradient(center, kShadowMargin + 1.0);
    QColor inner = ThemeManager::instance().color(ds::ColorToken::Text);
    inner.setAlpha(42);
    QColor outer = inner;
    outer.setAlpha(0);
    gradient.setColorAt(0.0, inner);
    gradient.setColorAt(0.18, inner);
    gradient.setColorAt(0.45, QColor(inner.red(), inner.green(), inner.blue(), 17));
    gradient.setColorAt(1.0, outer);
    painter.setPen(Qt::NoPen);
    painter.setBrush(gradient);
    const qreal x = center.x();
    const qreal y = center.y();
    const qreal r = kShadowMargin + 1.0;
    if (quadrant == 0) painter.drawRect(QRectF(x - r, y - r, r, r));
    if (quadrant == 1) painter.drawRect(QRectF(x, y - r, r, r));
    if (quadrant == 2) painter.drawRect(QRectF(x, y + 1, r, r));
    if (quadrant == 3) painter.drawRect(QRectF(x - r, y + 1, r, r));
}
}

WorkspaceSurface::WorkspaceSurface(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("WorkspaceSurface"));
    setAttribute(Qt::WA_StyledBackground, false);
    setAutoFillBackground(false);
}

void WorkspaceSurface::setFramelessCorners(bool enabled) {
    if (m_rounded == enabled) return;
    m_rounded = enabled;
    updateClip();
    update();
}

void WorkspaceSurface::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    updateClip();
}

void WorkspaceSurface::updateClip() {
    // 内容四角由 QSS 圆角与抗锯齿外框共同处理，不使用硬边窗口遮罩。
    update();
}

void WorkspaceSurface::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    const QRectF bounds = QRectF(rect()).adjusted(kShadowMargin, kShadowMargin,
                                                  -kShadowMargin, -kShadowMargin);
    QPainterPath outline;
    if (m_rounded) outline.addRoundedRect(bounds, kCornerRadius, kCornerRadius);
    else outline.addRect(bounds);

    // 直边使用连续线性渐变，圆角使用径向渐变；不再叠加离散描边。
    QColor near = ThemeManager::instance().color(ds::ColorToken::Text);
    near.setAlpha(32);
    QColor far = near;
    far.setAlpha(0);
    const auto edge = [&](const QPointF& start, const QPointF& end, const QRectF& area) {
        QLinearGradient gradient(start, end);
        gradient.setColorAt(0.0, near);
        gradient.setColorAt(0.35, QColor(near.red(), near.green(), near.blue(), 13));
        gradient.setColorAt(1.0, far);
        painter.setPen(Qt::NoPen);
        painter.setBrush(gradient);
        painter.drawRect(area);
    };
    edge(bounds.topLeft(), bounds.topLeft() - QPointF(0, kShadowMargin),
         QRectF(bounds.left(), 0, bounds.width(), kShadowMargin));
    edge(bounds.bottomLeft(), bounds.bottomLeft() + QPointF(0, kShadowMargin),
         QRectF(bounds.left(), bounds.bottom() + 1, bounds.width(), kShadowMargin - 1));
    edge(bounds.topLeft(), bounds.topLeft() - QPointF(kShadowMargin, 0),
         QRectF(0, bounds.top(), kShadowMargin, bounds.height()));
    edge(bounds.topRight(), bounds.topRight() + QPointF(kShadowMargin, 0),
         QRectF(bounds.right() + 1, bounds.top(), kShadowMargin - 1, bounds.height()));
    drawCornerShadow(painter, bounds.topLeft(), 0);
    drawCornerShadow(painter, bounds.topRight(), 1);
    drawCornerShadow(painter, bounds.bottomRight(), 2);
    drawCornerShadow(painter, bounds.bottomLeft(), 3);
    painter.setPen(QPen(ThemeManager::instance().color(ds::ColorToken::WindowBorder), 1.0));
    painter.setBrush(Qt::NoBrush);
    painter.drawPath(outline);
}

}  // namespace kl
