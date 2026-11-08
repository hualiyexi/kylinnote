#include "TaskDelegate.h"
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include "AppIcons.h"
#include "AppStyle.h"
#include "TaskListModel.h"
#include "design/ThemeManager.h"

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
        // R5: 笔记用 SVG 图标
        AppIcons::paint(p, IconId::Note, box, AppStyle::color(Color::Muted));
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
    const QRect titleRect(textLeft, row.top() + 6, textWidth, 20);
    p->drawText(titleRect,
                Qt::AlignLeft | Qt::AlignVCenter,
                p->fontMetrics().elidedText(i.data(Qt::DisplayRole).toString(), Qt::ElideRight, textWidth));

    p->setFont(AppStyle::font(AppStyle::Font::Small));
    p->setPen(AppStyle::color(completed ? Color::Faint : (overdue ? Color::Danger : Color::Muted)));
    const QRect metaRect(textLeft, row.top() + 26, textWidth, 16);
    p->drawText(metaRect,
                Qt::AlignLeft | Qt::AlignVCenter,
                p->fontMetrics().elidedText(
                    i.data(TaskListModel::MetaRole).toString(), Qt::ElideRight, textWidth));
    if (pinned)
        AppIcons::paint(p, IconId::Pin,
                        QRectF(row.right() - 28.0, row.center().y() - 7.0, 14.0, 14.0),
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
        // R5: 完成框命中区与选中区分离，30×30 热区
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
    // R3/R5: SVG 图标，选中时主题色
    const QColor iconColor = AppStyle::color(selected ? Color::Accent : Color::Muted);
    const QRectF iconRect(background.left() + 10.0, background.center().y() - 7.0, 14.0, 14.0);
    AppIcons::paint(p, static_cast<IconId>(i.data(IconRole).toInt()), iconRect, iconColor);

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
