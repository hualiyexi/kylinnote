#pragma once

#include <QColor>
#include <QElapsedTimer>
#include <QFrame>
#include <QTimer>
#include <functional>

class QLabel;
class QToolButton;

namespace kl {

// 固定配色的轻量提示岛。内容、可选动作与生命周期由本组件统一管理；
// 宿主只负责把它放在合适的窗格底部，不让业务代码拼样式或定时器。
class ToastIsland : public QFrame {
    Q_OBJECT
public:
    explicit ToastIsland(QWidget* parent = nullptr);

    void showMessage(const QString& text, int timeoutMs = 3000);
    void showAction(const QString& text, const QString& accessibleAction,
                    std::function<bool()> action, int timeoutMs = 3000);
    int remainingTime() const;
    bool isHovered() const { return m_hovered; }

signals:
    void actionFinished(bool succeeded);

protected:
    void enterEvent(QEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void paintEvent(QPaintEvent* event) override;

private:
    void present(const QString& text, int timeoutMs);
    void expire();

    QLabel* m_text = nullptr;
    QToolButton* m_action = nullptr;
    QTimer m_timer;
    QElapsedTimer m_clock;
    std::function<bool()> m_callback;
    int m_remaining = 0;
    bool m_hovered = false;
};

}  // namespace kl
