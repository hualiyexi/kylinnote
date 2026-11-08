#ifndef KYLINLIST_RESIZEGRIP_H
#define KYLINLIST_RESIZEGRIP_H

#include <QWidget>

class QPaintEvent;
class QMouseEvent;

namespace kl {

// 右下角缩放手柄：Qt 5.12 尚无 QWindow::startSystemResize，需自行计算
class ResizeGrip : public QWidget
{
    Q_OBJECT
public:
    explicit ResizeGrip(QWidget *parent = nullptr);

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void enterEvent(QEvent *event) override;
    void leaveEvent(QEvent *event) override;

private:
    QPoint m_pressGlobal;
    QSize  m_pressSize;
    bool   m_resizing = false;
};

} // namespace kl

#endif // KYLINLIST_RESIZEGRIP_H
