#pragma once

#include <QColor>
#include <QImage>
#include <QRectF>
#include <QSize>

namespace kl {

// 内容卡片与窗口外缘之间的透明区域；只裁内容圆角，不裁阴影。
constexpr int PopupShadowMargin = 24;

// 对偏移后的整张圆角 alpha 图做可分离高斯模糊：sigma 6、核半径 18、
// 峰值 alpha 0.20、纵向偏移 5。返回预乘格式，调用方按尺寸、卡片几何和主题影色缓存。
QImage popupShadow(const QSize& size, const QRectF& cardRect, qreal radius,
                   const QColor& color, int offsetY = 5);

}  // namespace kl
