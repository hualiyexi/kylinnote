#ifndef KYLINLIST_TITLEBAR_H
#define KYLINLIST_TITLEBAR_H

#include "../Common.h"

#include <QWidget>

class QToolButton;
class QMouseEvent;

namespace kl {

// 无边框窗口拖动条；关闭仅取消桌面投影，不删除任务。
class TitleBar : public QWidget {
    Q_OBJECT
public:
    explicit TitleBar(QWidget* parent = nullptr);

    void setColorIndicator(NoteColor color);
    void setHovered(bool hovered);

signals:
    void closeClicked();
    void colorClicked();
    void openClicked();

protected:
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;

private:
    QToolButton* m_openButton = nullptr;
    QToolButton* m_colorButton = nullptr;
    QToolButton* m_closeButton = nullptr;
    QPoint m_dragStartGlobal;
    QPoint m_dragStartWindowPos;
    bool m_dragging = false;
};

}  // namespace kl

#endif  // KYLINLIST_TITLEBAR_H
