#ifndef KYLINNOTE_TITLEBAR_H
#define KYLINNOTE_TITLEBAR_H

#include "../Common.h"

#include <QWidget>

class QToolButton;
class QMouseEvent;

namespace kn {

// 无边框窗口拖动条；关闭仅取消桌面投影，不删除任务。
class TitleBar : public QWidget {
    Q_OBJECT
public:
    explicit TitleBar(QWidget* parent = nullptr);

    void setColorIndicator(NoteColor color);

signals:
    void closeClicked();
    void colorClicked();
    void openClicked();

protected:
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;

private:
    QToolButton* m_colorButton = nullptr;
    QToolButton* m_closeButton = nullptr;
    QPoint m_dragStartGlobal;
    QPoint m_dragStartWindowPos;
    bool m_dragging = false;
};

}  // namespace kn

#endif  // KYLINNOTE_TITLEBAR_H
