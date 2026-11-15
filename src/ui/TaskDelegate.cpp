#include "TaskDelegate.h"

#include <QAbstractItemView>
#include <QHelpEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include "AppIcons.h"
#include "AppStyle.h"
#include "AppToolTip.h"
#include "ListAppearance.h"
#include "TaskListModel.h"
#include "design/ThemeManager.h"

namespace kl {
namespace {
using AppStyle::Color;
using AppStyle::Metric;

QRectF headerActionRect(const QRect& row) {
    return QRectF(row.right() - 58.0, row.center().y() - 13.0, 46.0, 26.0);
}

struct TaskLayout {
    QRectF background;
    QRectF colorBar;
    QRectF dragHandle;
    QRectF checkBox;
    QRectF title;
    QRectF list;
    QRectF date;
    QRectF pin;
};

TaskLayout taskLayout(const QStyleOptionViewItem& option, const QModelIndex& index) {
    const QRectF row = option.rect;
    const QFontMetrics smallMetrics(AppStyle::font(AppStyle::Font::Small));
    const int depth = qMax(0, index.data(TaskListModel::DepthRole).toInt());
    const qreal indent = depth * 24.0;
    const bool pinned = index.data(TaskListModel::PinnedRole).toBool();
    const bool deleted = index.data(TaskListModel::DeletedRole).toBool();
    const QString listName = index.data(TaskListModel::ListNameRole).toString();
    const QString date = index.data(TaskListModel::ScheduleRole).toString();

    TaskLayout layout;
    // 色条不随父子层级移动；拖动柄和悬浮背景在子任务中整体缩进一级。
    layout.colorBar = QRectF(row.left() + 16.0, row.top(), 3.0, row.height());
    layout.dragHandle = QRectF(row.left() + 5.0 + indent, row.center().y() - 6.0, 12.0, 12.0);
    layout.background = QRectF(row.left() + 20.0 + indent, row.top(),
                               qMax(0.0, row.width() - 38.0 - indent), row.height());
    layout.checkBox = QRectF(row.left() + 34.0 + indent, row.center().y() - 8.0, 16.0, 16.0);
    const qreal left = row.left() + 58.0 + indent;
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

struct NavigationLayout {
    QRectF background;
    QRectF icon;
    QRectF label;
    QString count;
};

NavigationLayout navigationLayout(const QStyleOptionViewItem& option, const QModelIndex& index) {
    const QRectF row = option.rect;
    NavigationLayout layout;
    layout.background = row.adjusted(10.0, 1.0, -10.0, -1.0);
    const int indent = index.data(NavigationDelegate::IndentRole).toInt();
    layout.icon = QRectF(layout.background.left() + 12.0 + indent,
                         layout.background.center().y() - 10.0, 20.0, 20.0);
    layout.count = index.data(NavigationDelegate::CountRole).toString();
    const QFont countFont = AppStyle::font(AppStyle::Font::Small);
    const int countWidth = QFontMetrics(countFont).horizontalAdvance(layout.count);
    const qreal right = layout.background.right() - 12.0 - countWidth;
    layout.label = QRectF(layout.icon.right() + 6.0, row.top(),
                          qMax(0.0, right - layout.icon.right() - 6.0), row.height());
    return layout;
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
    const TaskLayout layout = taskLayout(o, i);
    const QRectF background = layout.background;
    const qreal radius = AppStyle::metric(Metric::RadiusRow);

    if (selected || hovered) {
        p->setPen(Qt::NoPen);
        p->setBrush(AppStyle::color(selected ? Color::Selection : Color::Hover));
        p->drawRoundedRect(background, radius, radius);
    } else {
        p->setPen(QPen(AppStyle::color(Color::Separator), 1.0));
        p->drawLine(QPointF(layout.title.left(), row.bottom() - 0.5),
                    QPointF(row.right() - 14.0, row.bottom() - 0.5));
    }

    const QColor listColor(i.data(TaskListModel::ListColorRole).toString());
    if (!deleted && listColor.isValid())
        p->fillRect(layout.colorBar.toAlignedRect(), listColor);
    if (hovered && !deleted)
        AppIcons::paint(p, QStringLiteral("drag"), layout.dragHandle, AppStyle::color(Color::Faint));

    const QRectF box = layout.checkBox;
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
    return QSize(320, index.data(TaskListModel::HeaderRole).toBool()
                          ? 36 : AppStyle::metric(Metric::TaskRowHeight));
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
            const QString fullDate = index.data(TaskListModel::ScheduleTooltipRole).toString();
            if (fullDate != index.data(TaskListModel::ScheduleRole).toString()) text = fullDate;
        } else if (layout.list.contains(event->pos())) {
            target = layout.list.toAlignedRect();
            const QString listName = index.data(TaskListModel::ListNameRole).toString();
            const QFontMetrics metrics(AppStyle::font(AppStyle::Font::Small));
            if (metrics.horizontalAdvance(listName) > layout.list.width() - 18.0) text = listName;
        } else if (layout.title.contains(event->pos())) {
            target = layout.title.toAlignedRect();
            const QString title = index.data(Qt::DisplayRole).toString();
            const QFontMetrics metrics(AppStyle::font(AppStyle::Font::Body));
            if (metrics.horizontalAdvance(title) > layout.title.width()) text = title;
        }
    }
    if (!text.isEmpty())
        AppToolTip::showText(event->globalPos(), text, view->viewport(), target);
    else
        AppToolTip::hideText();
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
        const QRectF box = taskLayout(option, index).checkBox;
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
    const QRect row = o.rect;
    if (i.data(DividerRole).toBool()) {
        p->setRenderHint(QPainter::Antialiasing, false);
        p->setPen(QPen(ThemeManager::instance().color(ds::ColorToken::Divider), 1.0));
        const int y = row.top() + 16;
        p->drawLine(row.left() + 10, y, row.right() - 10, y);
        p->restore();
        return;
    }
    p->setRenderHint(QPainter::Antialiasing, true);
    const QString folderId = i.data(FolderIdRole).toString();
    if (i.data(SectionRole).toBool() && folderId.isEmpty()) {
        p->setFont(AppStyle::font(AppStyle::Font::Small));
        p->setPen(AppStyle::color(Color::Muted));
        p->drawText(row.adjusted(16, 8, 0, 0), Qt::AlignLeft | Qt::AlignVCenter,
                    i.data().toString());
        p->restore();
        return;
    }
    const NavigationLayout layout = navigationLayout(o, i);
    const bool selected = o.state & QStyle::State_Selected;
    const bool hovered = o.state & QStyle::State_MouseOver;
    const qreal radius = 10.0;
    if ((selected || hovered) && folderId.isEmpty()) {
        p->setPen(Qt::NoPen);
        p->setBrush(AppStyle::color(selected ? Color::Selection : Color::SidebarHover));
        p->drawRoundedRect(layout.background, radius, radius);
    }
    const QColor iconColor = AppStyle::color(selected ? Color::Accent : Color::Muted);
    const QString iconName = i.data(IconNameRole).toString();
    if (!iconName.isEmpty()) {
        TaskList list;
        list.icon = iconName;
        list.color = i.data(ListColorRole).toString();
        list.kind = static_cast<EntityKind>(i.data(ListKindRole).toInt());
        list.inbox = iconName == QLatin1String("inbox-list");
        ListAppearance::paint(p, list, layout.icon, iconColor);
    } else {
        AppIcons::paint(p, static_cast<IconId>(i.data(IconRole).toInt()), layout.icon, iconColor);
    }

    QFont labelFont = AppStyle::font(folderId.isEmpty() ? AppStyle::Font::Body
                                                        : AppStyle::Font::Small);
    if (selected || !folderId.isEmpty()) labelFont.setWeight(QFont::DemiBold);
    p->setFont(labelFont);
    p->setPen(AppStyle::color(folderId.isEmpty() ? Color::Text : Color::Muted));
    p->drawText(layout.label, Qt::AlignLeft | Qt::AlignVCenter,
                p->fontMetrics().elidedText(i.data().toString(), Qt::ElideRight,
                                            qMax(0, qRound(layout.label.width()))));

    p->setFont(AppStyle::font(AppStyle::Font::Small));
    p->setPen(AppStyle::color(selected ? Color::Muted : Color::Faint));
    p->drawText(QRectF(layout.background.left(), row.top(), layout.background.width() - 12.0,
                       row.height()), Qt::AlignRight | Qt::AlignVCenter, layout.count);
    if (o.state & QStyle::State_HasFocus) {
        p->setBrush(Qt::NoBrush);
        p->setPen(QPen(AppStyle::color(Color::Accent), 1.0));
        p->drawRoundedRect(layout.background.adjusted(0.5, 0.5, -0.5, -0.5), radius, radius);
    }
    p->restore();
}

QSize NavigationDelegate::sizeHint(const QStyleOptionViewItem&, const QModelIndex& i) const {
    if (i.data(DividerRole).toBool()) return QSize(160, 33);
    return QSize(160, i.data(SectionRole).toBool() ? 30 : AppStyle::metric(Metric::NavRowHeight));
}

bool NavigationDelegate::helpEvent(QHelpEvent* event, QAbstractItemView* view,
                                   const QStyleOptionViewItem& option, const QModelIndex& index) {
    if (!event || !view) return false;
    QString text;
    QRect target;
    if (!index.data(DividerRole).toBool() && !index.data(SectionRole).toBool()) {
        const NavigationLayout layout = navigationLayout(option, index);
        if (layout.label.contains(event->pos())) {
            target = layout.label.toAlignedRect();
            text = index.data(Qt::ToolTipRole).toString();
            if (text.isEmpty()
                && option.fontMetrics.horizontalAdvance(index.data(Qt::DisplayRole).toString())
                    > layout.label.width()) text = index.data(Qt::DisplayRole).toString();
        }
    }
    if (!text.isEmpty()) AppToolTip::showText(event->globalPos(), text, view->viewport(), target);
    else AppToolTip::hideText();
    return true;
}
}  // namespace kl
