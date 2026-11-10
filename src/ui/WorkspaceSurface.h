#pragma once

#include <QWidget>
#include <QImage>
#include <QColor>

namespace kl {

// 主窗口圆角内容容器。透明外边距留给应用自绘阴影，圆角路径抗锯齿绘制；
// 不使用 QWidget::setMask 的硬边裁剪。最大化时切换为直角。
class WorkspaceSurface : public QWidget {
public:
    explicit WorkspaceSurface(QWidget* parent = nullptr);
    void setFramelessCorners(bool enabled);
    void updateContentClip() { updateClip(); }

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    void updateClip();
    void updateShadow();
    QImage m_shadow;
    QColor m_shadowColor;
    bool m_rounded = true;
};

}  // namespace kl
