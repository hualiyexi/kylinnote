#include "TaskDelegate.h"
#include <QApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QStyle>
#include <QStyleOptionButton>
#include "TaskListModel.h"
namespace kn {
void TaskDelegate::paint(QPainter* p, const QStyleOptionViewItem& o, const QModelIndex& i) const {
    p->save();
    p->fillRect(o.rect, o.state & QStyle::State_Selected ? QColor("#EAF0FC") : QColor("#FFFFFF"));
    if (o.state & QStyle::State_MouseOver && !(o.state & QStyle::State_Selected))
        p->fillRect(o.rect, QColor("#F7F9FD"));
    const int x = o.rect.left(), y = o.rect.top();
    QStyleOptionButton checkbox;
    checkbox.rect = QRect(x + 16, y + 24, 20, 20);
    checkbox.state = QStyle::State_Enabled |
                     (i.data(TaskListModel::CompletedRole).toBool() ? QStyle::State_On : QStyle::State_Off);
    if (i.data(TaskListModel::DeletedRole).toBool())
        checkbox.state &= ~QStyle::State_Enabled;
    QApplication::style()->drawControl(QStyle::CE_CheckBox, &checkbox, p);
    const QRect title(x + 50, y + 13, o.rect.width() - 70, 25), meta(x + 50, y + 43, o.rect.width() - 68, 22);
    QFont f = o.font;
    f.setPointSize(11);
    f.setStrikeOut(i.data(TaskListModel::CompletedRole).toBool());
    p->setFont(f);
    p->setPen(i.data(TaskListModel::CompletedRole).toBool() ? QColor("#8390A2") : QColor("#23324B"));
    QString text = i.data(Qt::DisplayRole).toString();
    if (i.data(TaskListModel::FavoriteRole).toBool())
        text = QStringLiteral("★ ") + text;
    p->drawText(title,
                Qt::AlignLeft | Qt::AlignVCenter,
                p->fontMetrics().elidedText(text, Qt::ElideRight, title.width()));
    f.setPointSize(9);
    f.setStrikeOut(false);
    p->setFont(f);
    p->setPen(i.data(TaskListModel::OverdueRole).toBool() ? QColor("#AF3E38") : QColor("#64748B"));
    p->drawText(meta,
                Qt::AlignLeft | Qt::AlignVCenter,
                p->fontMetrics().elidedText(
                    i.data(TaskListModel::MetaRole).toString(), Qt::ElideRight, meta.width()));
    const int priority = i.data(TaskListModel::PriorityRole).toInt();
    if (priority > 0) {
        p->fillRect(QRect(x, y + 15, 3, 48), priority == 2 ? QColor("#BF5C51") : QColor("#C49A3A"));
    }
    p->setPen(QColor("#EBEFF5"));
    p->drawLine(x + 50, o.rect.bottom(), o.rect.right() - 12, o.rect.bottom());
    if (o.state & QStyle::State_HasFocus) {
        QPen pen(QColor("#9DB5DF"));
        pen.setStyle(Qt::DotLine);
        p->setPen(pen);
        p->drawRect(o.rect.adjusted(1, 1, -2, -2));
    }
    p->restore();
}
QSize TaskDelegate::sizeHint(const QStyleOptionViewItem&, const QModelIndex&) const {
    return QSize(340, 82);
}
bool TaskDelegate::editorEvent(QEvent* event,
                               QAbstractItemModel* model,
                               const QStyleOptionViewItem& option,
                               const QModelIndex& index) {
    if (!(model->flags(index) & Qt::ItemIsUserCheckable))
        return false;
    bool toggle = false;
    if (event->type() == QEvent::MouseButtonRelease) {
        auto* mouse = static_cast<QMouseEvent*>(event);
        toggle = mouse->button() == Qt::LeftButton &&
                 QRect(option.rect.topLeft() + QPoint(10, 17), QSize(32, 32)).contains(mouse->pos());
    }
    if (event->type() == QEvent::KeyPress)
        toggle = static_cast<QKeyEvent*>(event)->key() == Qt::Key_Space;
    if (toggle)
        return model->setData(
            index,
            index.data(Qt::CheckStateRole).toInt() == Qt::Checked ? Qt::Unchecked : Qt::Checked,
            Qt::CheckStateRole);
    return false;
}
}  // namespace kn
