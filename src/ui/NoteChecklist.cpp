#include "NoteChecklist.h"

#include <QFontMetrics>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>

namespace kn {

NoteChecklist::NoteChecklist(QWidget* parent) : QWidget(parent), m_ink(QStringLiteral("#39352D")) {
    setMouseTracking(true);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setAttribute(Qt::WA_OpaquePaintEvent, false);
}

void NoteChecklist::setItems(const QVector<ChecklistItem>& items) {
    m_items = items;
    m_hoverRow = -1;
    updateHeight();
    update();
}

void NoteChecklist::setInkColor(const QColor& color) {
    m_ink = color;
    update();
}

void NoteChecklist::updateHeight() {
    setFixedHeight(m_items.isEmpty() ? 0 : 10 + m_items.size() * 27);
}

int NoteChecklist::rowAt(const QPoint& position) const {
    const int row = (position.y() - 5) / 27;
    return row >= 0 && row < m_items.size() ? row : -1;
}

void NoteChecklist::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    QFont font = this->font();
    font.setPointSize(9);
    painter.setFont(font);
    const int available = qMax(0, width() - 40);
    for (int row = 0; row < m_items.size(); ++row) {
        const ChecklistItem& item = m_items.at(row);
        const int y = 5 + row * 27;
        if (row == m_hoverRow) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(QColor(0, 0, 0, 7));
            painter.drawRoundedRect(QRectF(0, y, width(), 26), 4, 4);
        }
        const QRect box(3, y + 6, 13, 13);
        painter.setPen(QPen(QColor(m_ink.red(), m_ink.green(), m_ink.blue(), 130), 1.2));
        painter.setBrush(item.done ? m_ink : Qt::transparent);
        painter.drawRoundedRect(box, 3, 3);
        if (item.done) {
            painter.setPen(QPen(palette().color(QPalette::Window), 1.5));
            painter.drawLine(QPointF(6, y + 12), QPointF(8.5, y + 15));
            painter.drawLine(QPointF(8.5, y + 15), QPointF(13.5, y + 8));
        }
        QColor text = m_ink;
        if (item.done)
            text.setAlpha(145);
        painter.setPen(text);
        font.setStrikeOut(item.done);
        painter.setFont(font);
        painter.drawText(QRect(25, y + 2, available, 22),
                         Qt::AlignVCenter | Qt::TextSingleLine,
                         painter.fontMetrics().elidedText(item.text, Qt::ElideRight, available));
        font.setStrikeOut(false);
        painter.setFont(font);
    }
}

void NoteChecklist::mouseReleaseEvent(QMouseEvent* event) {
    const int row = rowAt(event->pos());
    if (event->button() == Qt::LeftButton && row >= 0)
        emit itemToggled(row, !m_items.at(row).done);
    QWidget::mouseReleaseEvent(event);
}

void NoteChecklist::mouseMoveEvent(QMouseEvent* event) {
    const int row = rowAt(event->pos());
    if (row != m_hoverRow) {
        m_hoverRow = row;
        setCursor(row >= 0 ? Qt::PointingHandCursor : Qt::ArrowCursor);
        update();
    }
    QWidget::mouseMoveEvent(event);
}

void NoteChecklist::leaveEvent(QEvent* event) {
    m_hoverRow = -1;
    unsetCursor();
    update();
    QWidget::leaveEvent(event);
}

}  // namespace kn
