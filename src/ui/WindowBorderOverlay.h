#pragma once

#include <QWidget>

namespace kl {

// 位于所有工作区内容之上的透明边框层，只绘制轮廓，不拦截鼠标。
// 避免父容器底层边框被子控件背景覆盖。
class WindowBorderOverlay : public QWidget {
public:
    explicit WindowBorderOverlay(QWidget* parent = nullptr);
    void setRounded(bool rounded);

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    bool m_rounded = true;
};

}  // namespace kl
