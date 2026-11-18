#ifndef KYLINLIST_TITLEBAR_H
#define KYLINLIST_TITLEBAR_H

#include "../Common.h"
#include <QWidget>
#include <functional>

class QToolButton;
class QMouseEvent;

namespace kl {

// The empty part of a sticky note's top edge remains draggable when its tiny
// controls are hidden. Closing removes only the desktop projection.
class TitleBar : public QWidget {
    Q_OBJECT
public:
    explicit TitleBar(QWidget* parent = nullptr);
    void setColorIndicator(NoteColor color);
    void setHovered(bool hovered);
    void setPinned(bool pinned);
    void setMoveConstraint(std::function<QPoint(const QPoint&)> constraint);
signals:
    void closeClicked();
    void colorClicked();
    void pinClicked();
protected:
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
private:
    QToolButton* m_colorButton = nullptr;
    QToolButton* m_pinButton = nullptr;
    QToolButton* m_closeButton = nullptr;
    std::function<QPoint(const QPoint&)> m_moveConstraint;
    QPoint m_dragStartGlobal;
    QPoint m_dragStartWindowPos;
    bool m_dragging = false;
    bool m_pinned = false;
};

}  // namespace kl

#endif // KYLINLIST_TITLEBAR_H
