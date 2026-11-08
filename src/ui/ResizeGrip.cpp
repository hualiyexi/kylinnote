#include "ResizeGrip.h"

#include "../Common.h"

#include <QMouseEvent>
#include <QPainter>
#include <QPen>

namespace kl {

ResizeGrip::ResizeGrip(QWidget *parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("ResizeGrip"));
    setFixedSize(GripSize, GripSize);
    setCursor(Qt::SizeFDiagCursor);
    setAttribute(Qt::WA_StyledBackground, true);
    setToolTip(QStringLiteral("\u62d6\u62fd\u8c03\u6574\u5927\u5c0f"));
}

void ResizeGrip::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event);
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, false);

    QPen pen(QColor(0, 0, 0, 110), 1);
    pen.setCapStyle(Qt::RoundCap);
    painter.setPen(pen);

    const int s = GripSize;
    for (int i = 0; i < 3; ++i) {
        const int offset = 3 + i * 4;
        painter.drawLine(s - offset, s - 3, s - 3, s - offset);
    }
}

void ResizeGrip::mousePressEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(event);
        return;
    }
    QWidget *top = window();
    if (!top) {
        QWidget::mousePressEvent(event);
        return;
    }
    m_resizing = true;
    m_pressGlobal = event->globalPos();
    m_pressSize = top->size();
    event->accept();
}

void ResizeGrip::mouseMoveEvent(QMouseEvent *event)
{
    if (!m_resizing) {
        QWidget::mouseMoveEvent(event);
        return;
    }
    QWidget *top = window();
    if (!top) {
        return;
    }
    const QPoint delta = event->globalPos() - m_pressGlobal;
    const int w = qMax<int>(MinWidth, m_pressSize.width() + delta.x());
    const int h = qMax<int>(MinHeight, m_pressSize.height() + delta.y());
    top->resize(w, h);
    event->accept();
}

void ResizeGrip::mouseReleaseEvent(QMouseEvent *event)
{
    if (m_resizing && event->button() == Qt::LeftButton) {
        m_resizing = false;
        event->accept();
        return;
    }
    QWidget::mouseReleaseEvent(event);
}

void ResizeGrip::enterEvent(QEvent *event)
{
    setCursor(Qt::SizeFDiagCursor);
    QWidget::enterEvent(event);
}

void ResizeGrip::leaveEvent(QEvent *event)
{
    unsetCursor();
    QWidget::leaveEvent(event);
}

} // namespace kl
