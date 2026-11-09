#include "WorkspaceSurface.h"

#include <QPainter>
#include <QPainterPath>
#include <QResizeEvent>
#include "design/ThemeManager.h"

namespace kl {
namespace {
constexpr int kShadowMargin = 12;
constexpr qreal kCornerRadius = 10.0;
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
    // 圆角由 QPainter 抗锯齿路径负责；不使用硬边窗口遮罩。
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

    // 多层低透明度扩散形成四周轻阴影，不依赖系统窗口效果。
    for (int layer = 8; layer >= 1; --layer) {
        QColor shadow = ThemeManager::instance().color(ds::ColorToken::Text);
        shadow.setAlpha(3);
        painter.setPen(QPen(shadow, 1.0));
        painter.setBrush(Qt::NoBrush);
        QPainterPath glow;
        const qreal spread = layer * 1.25;
        const QRectF frame = bounds.adjusted(-spread, -spread, spread, spread);
        if (m_rounded) glow.addRoundedRect(frame, kCornerRadius + spread, kCornerRadius + spread);
        else glow.addRect(frame);
        painter.drawPath(glow);
    }
    painter.setPen(QPen(ThemeManager::instance().color(ds::ColorToken::Border), 1.0));
    painter.setBrush(Qt::NoBrush);
    painter.drawPath(outline);
}

}  // namespace kl
