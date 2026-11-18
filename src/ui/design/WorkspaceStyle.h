#pragma once

#include <QProxyStyle>

namespace kl {

// 原生焦点由统一中性状态表达，禁止 Fusion/平台样式再画蓝色焦点框。
class WorkspaceStyle final : public QProxyStyle {
public:
    WorkspaceStyle();
    void drawPrimitive(PrimitiveElement element, const QStyleOption* option,
                       QPainter* painter, const QWidget* widget = nullptr) const override;
};

} // namespace kl
