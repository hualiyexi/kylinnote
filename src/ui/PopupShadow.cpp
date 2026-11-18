#include "PopupShadow.h"

#include <QPainter>
#include <QVector>
#include <cmath>

namespace kl {
namespace {
constexpr qreal kSigma = 6.0;
constexpr int kKernelRadius = 18;
constexpr qreal kPeakAlpha = 0.20;
}  // namespace

QImage popupShadow(const QSize& size, const QRectF& cardRect, qreal radius,
                   const QColor& color, int offsetY) {
    if (size.isEmpty() || cardRect.isEmpty()) return QImage();
    QImage source(size, QImage::Format_ARGB32_Premultiplied);
    source.fill(Qt::transparent);
    {
        QPainter painter(&source);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setPen(Qt::NoPen);
        painter.setBrush(Qt::white);
        painter.drawRoundedRect(cardRect.translated(0, offsetY), radius, radius);
    }

    const int width = size.width();
    const int height = size.height();
    QVector<double> input(width * height);
    for (int y = 0; y < height; ++y) {
        const auto* row = reinterpret_cast<const QRgb*>(source.constScanLine(y));
        for (int x = 0; x < width; ++x) input[y * width + x] = qAlpha(row[x]) / 255.0;
    }

    QVector<double> kernel(kKernelRadius * 2 + 1);
    double sum = 0.0;
    for (int i = -kKernelRadius; i <= kKernelRadius; ++i) {
        const double value = std::exp(-(i * i) / (2.0 * kSigma * kSigma));
        kernel[i + kKernelRadius] = value;
        sum += value;
    }
    for (double& value : kernel) value /= sum;

    QVector<double> temporary(width * height, 0.0), output(width * height, 0.0);
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x)
            for (int k = -kKernelRadius; k <= kKernelRadius; ++k)
                temporary[y * width + x] +=
                    input[y * width + qBound(0, x + k, width - 1)] * kernel[k + kKernelRadius];
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x)
            for (int k = -kKernelRadius; k <= kKernelRadius; ++k)
                output[y * width + x] +=
                    temporary[qBound(0, y + k, height - 1) * width + x] * kernel[k + kKernelRadius];

    QImage shadow(size, QImage::Format_ARGB32);
    for (int y = 0; y < height; ++y) {
        auto* row = reinterpret_cast<QRgb*>(shadow.scanLine(y));
        for (int x = 0; x < width; ++x) {
            QColor pixel(color);
            pixel.setAlphaF(qMin(1.0, output[y * width + x] * kPeakAlpha));
            row[x] = pixel.rgba();
        }
    }
    return shadow.convertToFormat(QImage::Format_ARGB32_Premultiplied);
}

}  // namespace kl
