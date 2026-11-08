#include "TaskDelegate.h"
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include "AppStyle.h"
#include "TaskListModel.h"

namespace kl {
namespace {
using AppStyle::Color;
using AppStyle::Metric;

QRectF checkBoxRect(const QRect& row) {
    return QRectF(row.left() + 16.0, row.center().y() - 8.0, 16.0, 16.0);
}
void drawCheckMark(QPainter* p, const QRectF& box, const QColor& color, qreal width) {
    QPen pen(color, width, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    p->setPen(pen);
    p->setBrush(Qt::NoBrush);
    p->drawPolyline(QPolygonF() << QPointF(box.left() + box.width() * 0.24, box.center().y())
                                << QPointF(box.left() + box.width() * 0.43, box.bottom() - box.height() * 0.26)
                                << QPointF(box.right() - box.width() * 0.20, box.top() + box.height() * 0.24));
}
}  // namespace

void drawNavIcon(QPainter* painter, NavIcon icon, const QRectF& rect, const QColor& color) {
    if (icon == NavIcon::None)
        return;
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, true);
    QPen pen(color, 1.25, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    painter->setPen(pen);
    painter->setBrush(Qt::NoBrush);
    // 在 14x14 设计网格上绘制，再映射到目标区域。
    painter->translate(rect.topLeft());
    painter->scale(rect.width() / 14.0, rect.height() / 14.0);
    switch (icon) {
        case NavIcon::Inbox:
            painter->drawRoundedRect(QRectF(1.0, 3.5, 12.0, 9.0), 2.0, 2.0);
            painter->drawPolyline(QPolygonF() << QPointF(1.3, 8.0) << QPointF(4.6, 8.0)
                                              << QPointF(5.6, 9.8) << QPointF(8.4, 9.8)
                                              << QPointF(9.4, 8.0) << QPointF(12.7, 8.0));
            break;
        case NavIcon::Today:
        case NavIcon::Upcoming: {
            painter->drawRoundedRect(QRectF(1.5, 2.5, 11.0, 10.0), 2.0, 2.0);
            painter->drawLine(QPointF(1.8, 5.6), QPointF(12.2, 5.6));
            painter->drawLine(QPointF(4.5, 1.2), QPointF(4.5, 3.4));
            painter->drawLine(QPointF(9.5, 1.2), QPointF(9.5, 3.4));
            if (icon == NavIcon::Today) {
                painter->setBrush(color);
                painter->setPen(Qt::NoPen);
                painter->drawEllipse(QPointF(7.0, 8.8), 1.5, 1.5);
            } else {
                painter->drawLine(QPointF(4.0, 8.8), QPointF(10.0, 8.8));
            }
            break;
        }
        case NavIcon::Overdue:
            painter->drawEllipse(QPointF(7.0, 7.4), 5.4, 5.4);
            painter->drawPolyline(QPolygonF() << QPointF(7.0, 4.0) << QPointF(7.0, 7.4)
                                              << QPointF(9.4, 8.6));
            break;
        case NavIcon::Pin:
            painter->drawLine(QPointF(7.0, 2.0), QPointF(4.0, 5.0));
            painter->drawLine(QPointF(7.0, 2.0), QPointF(10.0, 5.0));
            painter->drawRoundedRect(QRectF(3.0, 4.5, 8.0, 3.0), 1.0, 1.0);
            painter->drawLine(QPointF(7.0, 7.5), QPointF(7.0, 12.5));
            painter->drawLine(QPointF(4.5, 10.0), QPointF(9.5, 10.0));
            break;
        case NavIcon::Note:
            painter->drawRoundedRect(QRectF(2.0, 1.5, 10.0, 11.0), 1.5, 1.5);
            painter->drawLine(QPointF(4.0, 4.5), QPointF(10.0, 4.5));
            painter->drawLine(QPointF(4.0, 7.0), QPointF(10.0, 7.0));
            painter->drawLine(QPointF(4.0, 9.5), QPointF(8.0, 9.5));
            break;
        case NavIcon::TaskList:
            painter->drawRoundedRect(QRectF(1.5, 2.0, 11.0, 10.0), 1.5, 1.5);
            drawCheckMark(painter, QRectF(2.5, 3.0, 5.0, 5.0), color, 1.2);
            painter->drawLine(QPointF(8.0, 5.0), QPointF(11.0, 5.0));
            painter->drawLine(QPointF(4.0, 9.0), QPointF(11.0, 9.0));
            break;
        case NavIcon::Done:
            painter->drawEllipse(QPointF(7.0, 7.0), 5.5, 5.5);
            drawCheckMark(painter, QRectF(2.6, 2.6, 8.8, 8.8), color, 1.4);
            break;
        case NavIcon::Trash:
            painter->drawLine(QPointF(2.4, 3.4), QPointF(11.6, 3.4));
            painter->drawPolyline(QPolygonF() << QPointF(5.4, 3.4) << QPointF(5.4, 2.0)
                                              << QPointF(8.6, 2.0) << QPointF(8.6, 3.4));
            painter->drawPolyline(QPolygonF() << QPointF(3.5, 3.4) << QPointF(4.2, 12.2)
                                              << QPointF(9.8, 12.2) << QPointF(10.5, 3.4));
            painter->drawLine(QPointF(6.0, 5.8), QPointF(6.2, 10.0));
            painter->drawLine(QPointF(8.0, 5.8), QPointF(7.8, 10.0));
            break;
        case NavIcon::Folder:
            painter->drawPolyline(QPolygonF() << QPointF(1.5, 11.8) << QPointF(1.5, 3.2)
                                              << QPointF(5.2, 3.2) << QPointF(6.4, 4.8)
                                              << QPointF(12.5, 4.8) << QPointF(12.5, 11.8)
                                              << QPointF(1.5, 11.8));
            break;
        case NavIcon::Abandoned:
            painter->drawEllipse(QPointF(7.0, 7.0), 5.5, 5.5);
            painter->drawLine(QPointF(3.2, 10.8), QPointF(10.8, 3.2));
            break;
        case NavIcon::None:
            break;
    }
    painter->restore();
}

void TaskDelegate::paint(QPainter* p, const QStyleOptionViewItem& o, const QModelIndex& i) const {
    p->save();
    p->setRenderHint(QPainter::Antialiasing, true);
    if (i.data(TaskListModel::HeaderRole).toBool()) {
        p->setFont(AppStyle::font(AppStyle::Font::Small));
        p->setPen(AppStyle::color(Color::Muted));
        p->drawText(o.rect.adjusted(16, 5, -12, 0), Qt::AlignLeft | Qt::AlignVCenter, i.data().toString());
        p->restore();
        return;
    }
    const bool selected = o.state & QStyle::State_Selected;
    const bool hovered = o.state & QStyle::State_MouseOver;
    const bool completed = i.data(TaskListModel::CompletedRole).toBool();
    const bool deleted = i.data(TaskListModel::DeletedRole).toBool();
    const bool pinned = i.data(TaskListModel::PinnedRole).toBool();
    const EntityKind kind = static_cast<EntityKind>(i.data(TaskListModel::KindRole).toInt());
    const bool overdue = i.data(TaskListModel::OverdueRole).toBool();
    const int priority = i.data(TaskListModel::PriorityRole).toInt();
    const QRect row = o.rect;
    const QRectF background = QRectF(row).adjusted(2.0, 2.0, -2.0, -2.0);
    const qreal radius = AppStyle::metric(Metric::RadiusRow);

    if (selected || hovered) {
        p->setPen(Qt::NoPen);
        p->setBrush(AppStyle::color(selected ? Color::Selection : Color::Hover));
        p->drawRoundedRect(background, radius, radius);
    } else {
        // 只有分隔发丝线，不画卡片边框。
        p->setPen(QPen(AppStyle::color(Color::Separator), 1.0));
        p->drawLine(QPointF(row.left() + 44.0, row.bottom() - 0.5),
                    QPointF(row.right() - 14.0, row.bottom() - 0.5));
    }
    if (priority > 0 && !deleted) {
        p->setPen(Qt::NoPen);
        p->setBrush(AppStyle::color(priority == 3 ? Color::Danger : Color::Warning));
        p->drawRoundedRect(QRectF(row.left() + 5.0, row.center().y() - 9.0, 3.0, 18.0), 1.5, 1.5);
    }

    const QRectF box = checkBoxRect(row);
    if (kind == EntityKind::Note) {
        drawNavIcon(p, NavIcon::Note, box, AppStyle::color(Color::Muted));
    } else if (completed) {
        p->setPen(Qt::NoPen);
        p->setBrush(AppStyle::color(Color::Accent));
        p->drawEllipse(box);
        drawCheckMark(p, box.adjusted(1.0, 1.0, -1.0, -1.0), AppStyle::color(Color::OnAccent), 1.7);
    } else {
        p->setBrush(Qt::NoBrush);
        p->setPen(QPen(AppStyle::color(deleted ? Color::Separator : Color::Faint), 1.4));
        p->drawEllipse(box);
    }

    const int textLeft = row.left() + 44;
    const int textWidth = row.right() - textLeft - (pinned ? 36 : 18);
    QFont titleFont = AppStyle::font(AppStyle::Font::Body);
    titleFont.setStrikeOut(completed);
    p->setFont(titleFont);
    p->setPen(AppStyle::color(completed ? Color::Muted : Color::Text));
    const QRect titleRect(textLeft, row.top() + 8, textWidth, 20);
    p->drawText(titleRect,
                Qt::AlignLeft | Qt::AlignVCenter,
                p->fontMetrics().elidedText(i.data(Qt::DisplayRole).toString(), Qt::ElideRight, textWidth));

    p->setFont(AppStyle::font(AppStyle::Font::Small));
    p->setPen(AppStyle::color(completed ? Color::Faint : (overdue ? Color::Danger : Color::Muted)));
    const QRect metaRect(textLeft, row.top() + 30, textWidth, 16);
    p->drawText(metaRect,
                Qt::AlignLeft | Qt::AlignVCenter,
                p->fontMetrics().elidedText(
                    i.data(TaskListModel::MetaRole).toString(), Qt::ElideRight, textWidth));
    if (pinned)
        drawNavIcon(p, NavIcon::Pin, QRectF(row.right() - 28.0, row.center().y() - 7.0, 14.0, 14.0),
                    AppStyle::color(Color::Accent));

    if (o.state & QStyle::State_HasFocus) {
        p->setBrush(Qt::NoBrush);
        p->setPen(QPen(AppStyle::color(Color::Accent), 1.0));
        p->drawRoundedRect(background.adjusted(0.5, 0.5, -0.5, -0.5), radius, radius);
    }
    p->restore();
}
QSize TaskDelegate::sizeHint(const QStyleOptionViewItem&, const QModelIndex& index) const {
    return QSize(320, index.data(TaskListModel::HeaderRole).toBool() ? 30 : AppStyle::metric(Metric::TaskRowHeight));
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
        const QRectF box = checkBoxRect(option.rect);
        toggle = mouse->button() == Qt::LeftButton &&
                 QRectF(box.center().x() - 15.0, box.center().y() - 15.0, 30.0, 30.0).contains(mouse->pos());
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

void NavigationDelegate::paint(QPainter* p, const QStyleOptionViewItem& o, const QModelIndex& i) const {
    p->save();
    p->setRenderHint(QPainter::Antialiasing, true);
    const QRect row = o.rect;
    if (i.data(SectionRole).toBool()) {
        p->setFont(AppStyle::font(AppStyle::Font::Small));
        p->setPen(AppStyle::color(Color::Muted));
        p->drawText(row.adjusted(16, 8, 0, 0), Qt::AlignLeft | Qt::AlignVCenter, i.data().toString());
        p->restore();
        return;
    }
    const bool selected = o.state & QStyle::State_Selected;
    const bool hovered = o.state & QStyle::State_MouseOver;
    const QRectF background = QRectF(row).adjusted(6.0, 1.5, -6.0, -1.5);
    const qreal radius = AppStyle::metric(Metric::RadiusRow);
    if (selected || hovered) {
        p->setPen(Qt::NoPen);
        p->setBrush(AppStyle::color(selected ? Color::Selection : Color::SidebarHover));
        p->drawRoundedRect(background, radius, radius);
    }
    const QColor iconColor = AppStyle::color(selected ? Color::Accent : Color::Muted);
    const QRectF iconRect(background.left() + 10.0, background.center().y() - 7.0, 14.0, 14.0);
    drawNavIcon(p, static_cast<NavIcon>(i.data(IconRole).toInt()), iconRect, iconColor);

    QFont labelFont = AppStyle::font(AppStyle::Font::Body);
    if (selected)
        labelFont.setWeight(QFont::DemiBold);
    p->setFont(labelFont);
    p->setPen(AppStyle::color(Color::Text));
    const QString count = i.data(CountRole).toString();
    const QFont countFont = AppStyle::font(AppStyle::Font::Small);
    const int countWidth = QFontMetrics(countFont).horizontalAdvance(count);
    const QRectF labelRect(iconRect.right() + 10.0,
                           row.top(),
                           background.right() - iconRect.right() - 22.0 - countWidth,
                           row.height());
    p->drawText(labelRect,
                Qt::AlignLeft | Qt::AlignVCenter,
                p->fontMetrics().elidedText(i.data().toString(), Qt::ElideRight, int(labelRect.width())));

    p->setFont(countFont);
    p->setPen(AppStyle::color(selected ? Color::Muted : Color::Faint));
    p->drawText(QRectF(background.left(), row.top(), background.width() - 12.0, row.height()),
                Qt::AlignRight | Qt::AlignVCenter,
                count);
    if (o.state & QStyle::State_HasFocus) {
        p->setBrush(Qt::NoBrush);
        p->setPen(QPen(AppStyle::color(Color::Accent), 1.0));
        p->drawRoundedRect(background.adjusted(0.5, 0.5, -0.5, -0.5), radius, radius);
    }
    p->restore();
}
QSize NavigationDelegate::sizeHint(const QStyleOptionViewItem&, const QModelIndex& i) const {
    return QSize(160, i.data(SectionRole).toBool() ? 30 : AppStyle::metric(Metric::NavRowHeight));
}
}  // namespace kl
