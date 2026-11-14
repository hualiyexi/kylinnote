#include "TaskDelegate.h"

#include <QAbstractItemView>
#include <QHelpEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QToolTip>
#include "AppIcons.h"
#include "AppStyle.h"
#include "ListAppearance.h"
#include "TaskListModel.h"
#include "design/ThemeManager.h"

namespace kl {
namespace {
using AppStyle::Color;
using AppStyle::Metric;

QRectF checkBoxRect(const QRect& row) {
    return QRectF(row.left() + 16.0, row.center().y() - 8.0, 16.0, 16.0);
}

QRectF headerActionRect(const QRect& row) {
    return QRectF(row.right() - 58.0, row.center().y() - 13.0, 46.0, 26.0);
}

struct TaskLayout {
    QRectF title;
    QRectF list;
    QRectF date;
    QRectF pin;
};

TaskLayout taskLayout(const QStyleOptionViewItem& option, const QModelIndex& index) {
    const QRectF row = option.rect;
    const QFontMetrics smallMetrics(AppStyle::font(AppStyle::Font::Small));
    const bool pinned = index.data(TaskListModel::PinnedRole).toBool();
    const bool deleted = index.data(TaskListModel::DeletedRole).toBool();
    const QString listName = index.data(TaskListModel::ListNameRole).toString();
    const QString date = index.data(TaskListModel::ScheduleRole).toString();

    TaskLayout layout;
    const qreal left = row.left() + 44.0;
    const qreal titleFloor = qMin(160.0, qMax(0.0, row.width() - 56.0) * 0.5);
    const qreal metadataWidth = qMax(0.0, row.width() - 56.0 - titleFloor - (pinned ? 24.0 : 0.0));
    qreal right = row.right() - 12.0;
    if (pinned) {
        layout.pin = QRectF(right - 18.0, row.center().y() - 7.0, 14.0, 14.0);
        right -= 24.0;
    }
    if (!deleted && !date.isEmpty()) {
        const qreal width = qMin(qMin(112.0, smallMetrics.horizontalAdvance(date) + 8.0),
                                metadataWidth * 0.55);
        layout.date = QRectF(right - width, row.top(), width, row.height());
        right -= width + 8.0;
    }
    if (!deleted && !listName.isEmpty()) {
        const qreal nameWidth = qMin(72.0, smallMetrics.horizontalAdvance(listName) + 0.0);
        const qreal width = qMin(nameWidth + 27.0, qMax(0.0, right - left - titleFloor - 8.0));
        if (width >= 22.0) {
            layout.list = QRectF(right - width, row.top(), width, row.height());
            right -= width + 8.0;
        }
    }
    layout.title = QRectF(left, row.top(), qMax(0.0, right - left), row.height());
    return layout;
}

TaskList listFromIndex(const QModelIndex& index) {
    TaskList list;
    list.id = index.data(TaskListModel::ListIdRole).toString();
    list.name = index.data(TaskListModel::ListNameRole).toString();
    list.icon = index.data(TaskListModel::ListIconRole).toString();
    list.color = index.data(TaskListModel::ListColorRole).toString();
    list.kind = static_cast<EntityKind>(index.data(TaskListModel::KindRole).toInt());
    list.inbox = list.icon == QLatin1String("inbox-list");
    return list;
}

}  // namespace

void TaskDelegate::paint(QPainter* p, const QStyleOptionViewItem& o, const QModelIndex& i) const {
    p->save();
    p->setRenderHint(QPainter::Antialiasing, true);
    if (i.data(TaskListModel::HeaderRole).toBool()) {
        const QRect row = o.rect;
        const QFont titleFont = AppStyle::font(AppStyle::Font::Small);
        p->setFont(titleFont);
        p->setPen(AppStyle::color(Color::Muted));
        const QString title = i.data().toString();
        p->drawText(QRect(row.left() + 16, row.top(), row.width() - 90, row.height()),
                    Qt::AlignLeft | Qt::AlignVCenter, title);
        const int count = i.data(TaskListModel::HeaderEntityIdsRole).toStringList().size();
        if (count > 0) {
            const int titleWidth = p->fontMetrics().horizontalAdvance(title);
            p->setPen(AppStyle::color(Color::Faint));
            p->drawText(QRect(row.left() + 24 + titleWidth, row.top(), 36, row.height()),
                        Qt::AlignLeft | Qt::AlignVCenter, QString::number(count));
        }
        if (!i.data(TaskListModel::HeaderActionRole).toString().isEmpty()) {
            const QRectF action = headerActionRect(row);
            if (o.state & QStyle::State_MouseOver) {
                p->setPen(Qt::NoPen);
                p->setBrush(ThemeManager::instance().color(ds::ColorToken::PrimarySoft));
                p->drawRoundedRect(action, 5, 5);
            }
            p->setFont(AppStyle::font(AppStyle::Font::Small));
            p->setPen(ThemeManager::instance().color(ds::ColorToken::Primary));
            p->drawText(action, Qt::AlignCenter, QStringLiteral("顺延"));
        }
        p->restore();
        return;
    }
    const bool selected = o.state & QStyle::State_Selected;
    const bool hovered = o.state & QStyle::State_MouseOver;
    const bool completed = i.data(TaskListModel::CompletedRole).toBool();
    const bool deleted = i.data(TaskListModel::DeletedRole).toBool();
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
        p->setPen(QPen(AppStyle::color(Color::Separator), 1.0));
        p->drawLine(QPointF(row.left() + 44.0, row.bottom() - 0.5),
                    QPointF(row.right() - 14.0, row.bottom() - 0.5));
    }

    const QColor listColor(i.data(TaskListModel::ListColorRole).toString());
    if (!deleted && listColor.isValid()) {
        p->setPen(Qt::NoPen);
        p->setBrush(listColor);
        p->drawRoundedRect(QRectF(row.left() + 5.0, row.center().y() - 9.0, 3.0, 18.0), 1.5, 1.5);
    }

    const QRectF box = checkBoxRect(row);
    if (kind == EntityKind::Note) {
        AppIcons::paint(p, IconId::Note, box, AppStyle::color(Color::Muted));
    } else if (completed) {
        AppIcons::paint(p, QStringLiteral("completed"), box, AppStyle::color(Color::Muted));
    } else {
        ds::ColorToken token = deleted ? ds::ColorToken::TextTertiary : ds::ColorToken::TextSecondary;
        if (!deleted && priority > 0) {
            token = priority == 3 ? ds::ColorToken::PriorityHigh
                : priority == 2 ? ds::ColorToken::PriorityMedium : ds::ColorToken::PriorityLow;
        }
        AppIcons::paint(p, hovered && !deleted ? QStringLiteral("hover-checkbox")
                                             : QStringLiteral("checkbox"),
                        box, ThemeManager::instance().color(token));
    }

    const TaskLayout layout = taskLayout(o, i);
    QFont titleFont = AppStyle::font(AppStyle::Font::Body);
    titleFont.setStrikeOut(completed);
    p->setFont(titleFont);
    p->setPen(AppStyle::color(completed ? Color::Muted : Color::Text));
    p->drawText(layout.title, Qt::AlignLeft | Qt::AlignVCenter,
                p->fontMetrics().elidedText(i.data().toString(), Qt::ElideRight,
                                            qMax(0, qRound(layout.title.width()))));

    p->setFont(AppStyle::font(AppStyle::Font::Small));
    if (!layout.list.isNull()) {
        const TaskList list = listFromIndex(i);
        const QRectF glyph(layout.list.left(), layout.list.center().y() - 7.0, 14.0, 14.0);
        ListAppearance::paint(p, list, glyph, AppStyle::color(Color::Muted));
        p->setPen(AppStyle::color(completed ? Color::Faint : Color::Muted));
        const QRectF nameRect(glyph.right() + 4.0, layout.list.top(),
                              layout.list.right() - glyph.right() - 4.0, layout.list.height());
        p->drawText(nameRect, Qt::AlignLeft | Qt::AlignVCenter,
                    p->fontMetrics().elidedText(list.name, Qt::ElideRight,
                                                qMax(0, qRound(nameRect.width()))));
    }
    if (!layout.date.isNull()) {
        p->setPen(AppStyle::color(completed ? Color::Faint
                                            : (overdue ? Color::Danger : Color::Muted)));
        p->drawText(layout.date, Qt::AlignLeft | Qt::AlignVCenter,
                    p->fontMetrics().elidedText(i.data(TaskListModel::ScheduleRole).toString(),
                                                Qt::ElideRight, qRound(layout.date.width())));
    }
    if (!layout.pin.isNull())
        AppIcons::paint(p, IconId::Pin, layout.pin, AppStyle::color(Color::Accent));

    if (o.state & QStyle::State_HasFocus) {
        p->setBrush(Qt::NoBrush);
        p->setPen(QPen(AppStyle::color(Color::Accent), 1.0));
        p->drawRoundedRect(background.adjusted(0.5, 0.5, -0.5, -0.5), radius, radius);
    }
    p->restore();
}

QSize TaskDelegate::sizeHint(const QStyleOptionViewItem&, const QModelIndex& index) const {
    return QSize(320, index.data(TaskListModel::HeaderRole).toBool() ? 36 : 40);
}

bool TaskDelegate::helpEvent(QHelpEvent* event, QAbstractItemView* view,
                             const QStyleOptionViewItem& option, const QModelIndex& index) {
    if (!event || !view) return false;
    QString text;
    QRect target;
    if (index.data(TaskListModel::HeaderRole).toBool()) {
        target = headerActionRect(option.rect).toAlignedRect();
        if (!index.data(TaskListModel::HeaderActionRole).toString().isEmpty()
            && target.contains(event->pos())) text = QStringLiteral("将已过期任务顺延至今天");
    } else {
        const TaskLayout layout = taskLayout(option, index);
        if (layout.date.contains(event->pos())) {
            target = layout.date.toAlignedRect();
            text = index.data(TaskListModel::ScheduleTooltipRole).toString();
        } else if (layout.list.contains(event->pos())) {
            target = layout.list.toAlignedRect();
            text = index.data(TaskListModel::ListNameRole).toString();
        }
    }
    if (text.isEmpty()) return QStyledItemDelegate::helpEvent(event, view, option, index);
    QToolTip::showText(event->globalPos(), text, view->viewport(), target);
    return true;
}

bool TaskDelegate::editorEvent(QEvent* event,
                               QAbstractItemModel* model,
                               const QStyleOptionViewItem& option,
                               const QModelIndex& index) {
    if (event->type() == QEvent::MouseButtonRelease) {
        auto* mouse = static_cast<QMouseEvent*>(event);
        if (mouse->button() == Qt::LeftButton) {
            if (index.data(TaskListModel::HeaderRole).toBool()) {
                if (!index.data(TaskListModel::HeaderActionRole).toString().isEmpty()
                    && headerActionRect(option.rect).contains(mouse->pos())) {
                    emit postponeActivated(
                        index.data(TaskListModel::HeaderEntityIdsRole).toStringList());
                    return true;
                }
                return false;
            }
            const TaskLayout layout = taskLayout(option, index);
            if (!layout.date.isNull() && layout.date.contains(mouse->pos())) {
                emit dateActivated(index.data(TaskListModel::IdRole).toString(),
                                   layout.date.toAlignedRect());
                return true;
            }
            if (!layout.list.isNull() && layout.list.contains(mouse->pos())) {
                emit listActivated(index.data(TaskListModel::ListIdRole).toString());
                return true;
            }
        }
    }
    if (!(model->flags(index) & Qt::ItemIsUserCheckable)) return false;
    bool toggle = false;
    if (event->type() == QEvent::MouseButtonRelease) {
        auto* mouse = static_cast<QMouseEvent*>(event);
        const QRectF box = checkBoxRect(option.rect);
        toggle = mouse->button() == Qt::LeftButton &&
                 QRectF(box.center().x() - 15.0, box.center().y() - 15.0, 30.0, 30.0)
                     .contains(mouse->pos());
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
    const QString folderId = i.data(FolderIdRole).toString();
    if (i.data(SectionRole).toBool() && folderId.isEmpty()) {
        p->setFont(AppStyle::font(AppStyle::Font::Small));
        p->setPen(AppStyle::color(Color::Muted));
        p->drawText(row.adjusted(16, 8, 0, 0), Qt::AlignLeft | Qt::AlignVCenter,
                    i.data().toString());
        p->restore();
        return;
    }
    const bool selected = o.state & QStyle::State_Selected;
    const bool hovered = o.state & QStyle::State_MouseOver;
    const QRectF background = QRectF(row).adjusted(6.0, 1.5, -6.0, -1.5);
    const qreal radius = AppStyle::metric(Metric::RadiusRow);
    if ((selected || hovered) && folderId.isEmpty()) {
        p->setPen(Qt::NoPen);
        p->setBrush(AppStyle::color(selected ? Color::Selection : Color::SidebarHover));
        p->drawRoundedRect(background, radius, radius);
    }
    const int indent = i.data(IndentRole).toInt();
    const QColor iconColor = AppStyle::color(selected ? Color::Accent : Color::Muted);
    const QRectF iconRect(background.left() + 10.0 + indent,
                          background.center().y() - 9.0, 18.0, 18.0);
    const QString iconName = i.data(IconNameRole).toString();
    if (!iconName.isEmpty()) {
        TaskList list;
        list.icon = iconName;
        list.color = i.data(ListColorRole).toString();
        list.kind = static_cast<EntityKind>(i.data(ListKindRole).toInt());
        list.inbox = iconName == QLatin1String("inbox-list");
        ListAppearance::paint(p, list, iconRect, iconColor);
    } else {
        AppIcons::paint(p, static_cast<IconId>(i.data(IconRole).toInt()), iconRect, iconColor);
    }

    QFont labelFont = AppStyle::font(folderId.isEmpty() ? AppStyle::Font::Body
                                                        : AppStyle::Font::Small);
    if (selected || !folderId.isEmpty()) labelFont.setWeight(QFont::DemiBold);
    p->setFont(labelFont);
    p->setPen(AppStyle::color(folderId.isEmpty() ? Color::Text : Color::Muted));
    const QString count = i.data(CountRole).toString();
    const QFont countFont = AppStyle::font(AppStyle::Font::Small);
    const int countWidth = QFontMetrics(countFont).horizontalAdvance(count);
    const QRectF labelRect(iconRect.right() + 8.0, row.top(),
                           background.right() - iconRect.right() - 20.0 - countWidth - indent,
                           row.height());
    p->drawText(labelRect, Qt::AlignLeft | Qt::AlignVCenter,
                p->fontMetrics().elidedText(i.data().toString(), Qt::ElideRight,
                                            qMax(0, qRound(labelRect.width()))));

    p->setFont(countFont);
    p->setPen(AppStyle::color(selected ? Color::Muted : Color::Faint));
    p->drawText(QRectF(background.left(), row.top(), background.width() - 12.0, row.height()),
                Qt::AlignRight | Qt::AlignVCenter, count);
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
