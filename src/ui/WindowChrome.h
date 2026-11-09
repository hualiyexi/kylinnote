#pragma once

#include <QPointer>
#include <QWidget>

class QHBoxLayout;
class QMouseEvent;
class QToolButton;

namespace kl {

// 主窗口的顶部命中区：背景由所在列提供，不画独立标题栏。
class WindowDragArea : public QWidget {
public:
    explicit WindowDragArea(QWidget* parent = nullptr);

protected:
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;

private:
    QPoint m_pressGlobal;
    QPoint m_windowPosition;
    bool m_dragging = false;
};

// 右上角三个窗口按钮。关闭主窗口仍走 TaskPanel::closeEvent 的托盘语义。
class WindowControls : public QWidget {
    Q_OBJECT
public:
    explicit WindowControls(QWidget* parent = nullptr);
    void refreshIcons();

private:
    QPointer<QWidget> m_window;
    QToolButton* m_minimize = nullptr;
    QToolButton* m_maximize = nullptr;
    QToolButton* m_close = nullptr;
};

}  // namespace kl
