#include "WorkspaceSurface.h"

#include <QPainter>
#include <QPainterPath>
#include <QResizeEvent>
#include <cmath>
#include "design/ThemeManager.h"

namespace kl {
namespace {
constexpr int kShadowMargin = 18;
constexpr qreal kCornerRadius = 10.0;

// 对整张圆角 alpha 图做可分离高斯模糊。直边与转角共享同一连续场，
// 不存在分区渐变在圆角旁叠加形成的直角接缝。
QImage blurredShadow(const QSize& size, bool rounded) {
    if (size.isEmpty()) return QImage();
    QImage source(size, QImage::Format_ARGB32_Premultiplied);
    source.fill(Qt::transparent);
    QPainter painter(&source);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(Qt::NoPen);
    painter.setBrush(Qt::white);
    const QRectF bounds = QRectF(QPointF(kShadowMargin, kShadowMargin),
                                 QSizeF(size.width() - 2.0 * kShadowMargin,
                                        size.height() - 2.0 * kShadowMargin));
    if (rounded) painter.drawRoundedRect(bounds, kCornerRadius, kCornerRadius);
    else painter.drawRect(bounds);
    painter.end();

    const int width = size.width();
    const int height = size.height();
    QVector<double> input(width * height);
    for (int y = 0; y < height; ++y) {
        const auto* row = reinterpret_cast<const QRgb*>(source.constScanLine(y));
        for (int x = 0; x < width; ++x) input[y * width + x] = qAlpha(row[x]) / 255.0;
    }
    const double sigma = 5.2;
    const int radius = 15;
    QVector<double> kernel(radius * 2 + 1);
    double sum = 0.0;
    for (int i = -radius; i <= radius; ++i) {
        const double value = std::exp(-(i * i) / (2.0 * sigma * sigma));
        kernel[i + radius] = value;
        sum += value;
    }
    for (double& value : kernel) value /= sum;
    QVector<double> temporary(width * height, 0.0), output(width * height, 0.0);
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x)
            for (int k = -radius; k <= radius; ++k)
                temporary[y * width + x] += input[y * width + qBound(0, x + k, width - 1)] * kernel[k + radius];
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x)
            for (int k = -radius; k <= radius; ++k)
                output[y * width + x] += temporary[qBound(0, y + k, height - 1) * width + x] * kernel[k + radius];

    QImage shadow(size, QImage::Format_ARGB32);
    const QColor color = ThemeManager::instance().color(ds::ColorToken::Text);
    for (int y = 0; y < height; ++y) {
        auto* row = reinterpret_cast<QRgb*>(shadow.scanLine(y));
        for (int x = 0; x < width; ++x) {
            QColor pixel(color);
            pixel.setAlphaF(qMin(1.0, output[y * width + x] * 0.28));
            row[x] = pixel.rgba();
        }
    }
    return shadow.convertToFormat(QImage::Format_ARGB32_Premultiplied);
}
}  // namespace

WorkspaceSurface::WorkspaceSurface(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("WorkspaceSurface"));
    setAttribute(Qt::WA_StyledBackground, false);
    setAutoFillBackground(false);
}

void WorkspaceSurface::setFramelessCorners(bool enabled) {
    if (m_rounded == enabled) return;
    m_rounded = enabled;
    updateClip();
}

void WorkspaceSurface::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    updateClip();
}

void WorkspaceSurface::updateClip() {
    updateShadow();
    update();
}

void WorkspaceSurface::updateShadow() {
    m_shadow = blurredShadow(size(), m_rounded);
}

void WorkspaceSurface::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    if (!m_shadow.isNull()) painter.drawImage(0, 0, m_shadow);
    const QRectF bounds = QRectF(rect()).adjusted(kShadowMargin, kShadowMargin,
                                                  -kShadowMargin, -kShadowMargin);
    QPainterPath outline;
    if (m_rounded) outline.addRoundedRect(bounds, kCornerRadius, kCornerRadius);
    else outline.addRect(bounds);
    painter.setPen(QPen(ThemeManager::instance().color(ds::ColorToken::WindowBorder), 1.2));
    painter.setBrush(Qt::NoBrush);
    painter.drawPath(outline);
}

}  // namespace kl
