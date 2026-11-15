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
#include "design/TypeSystem.h"

namespace kl {
namespace {
using AppStyle::Color;
using AppStyle::Metric;

QRectF headerActionRect(const QRect& row) {
    return QRectF(row.right() - 58.0, row.center().y() - 13.0, 46.0, 26.0);
}

// 与滴答分区头一致：左侧 20px 起画 12px 三角，标题从 36px 起排。
QRectF sectionArrowRect(const QRect& row) {
    return QRectF(row.left() + 20.0, row.center().y() - 6.0, 12.0, 12.0);
}
constexpr qreal kSectionTitleLeft = 36.0;

// 滴答用同一个下三角旋转表示收起，避免再维护一套右向图标。
void paintTriangle(QPainter* p, const QRectF& rect, bool expanded, const QColor& color) {
    p->save();
    p->translate(rect.center());
    if (!expanded) p->rotate(-90.0);
    AppIcons::paint(p, QStringLiteral("thin-triangle-down"),
                    QRectF(-rect.width() / 2.0, -rect.height() / 2.0,
                           rect.width(), rect.height()),
                    color);
    p->restore();
}

struct TaskLayout {
    QRectF background;
    QRectF colorBar;
    QRectF dragHandle;
    QRectF disclosure;
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
    if (index.data(TaskListModel::HasChildrenRole).toBool())
        layout.disclosure = QRectF(row.left() + 20.0 + indent,
                                   row.center().y() - 8.0, 16.0, 16.0);
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
    QRectF disclosure;
    QRectF icon;
    QRectF label;
    QString count;
};

NavigationLayout navigationLayout(const QStyleOptionViewItem& option, const QModelIndex& index) {
    const QRectF row = option.rect;
    NavigationLayout layout;
    layout.background = row.adjusted(10.0, 1.0, -10.0, -1.0);
    const int indent = index.data(NavigationDelegate::IndentRole).toInt();
    const bool folder = !index.data(NavigationDelegate::FolderIdRole).toString().isEmpty();
    // 滴答文件夹行：箭头占行首 16px，图标盒 20px（12px 起），再空 6px 到文字。
    if (folder)
        layout.disclosure = QRectF(layout.background.left(),
                                   layout.background.center().y() - 8.0, 16.0, 16.0);
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
        const QFont titleFont = ds::qfont(ds::FontScene::ListSection);
        p->setFont(titleFont);
        p->setPen(AppStyle::color(Color::Text));
        const QString title = i.data().toString();
        const bool collapsed = i.data(TaskListModel::SectionCollapsedRole).toBool();
        const QColor arrowColor = AppStyle::color(
            (o.state & QStyle::State_MouseOver) ? Color::Muted : Color::Faint);
        paintTriangle(p, sectionArrowRect(row), !collapsed, arrowColor);
        qreal textLeft = row.left() + kSectionTitleLeft;
        if (i.data(TaskListModel::SectionKeyRole).toString() == QLatin1String("pinned")) {
            const QRectF icon(textLeft, row.center().y() - 8.0, 16.0, 16.0);
            AppIcons::paint(p, IconId::PinSection, icon, AppStyle::color(Color::Star));
            textLeft = icon.right() + 5.0;
        }
        p->drawText(QRectF(textLeft, row.top(), qMax(0.0, row.width() - textLeft), row.height()),
                    Qt::AlignLeft | Qt::AlignVCenter, title);
        const int count = i.data(TaskListModel::HeaderEntityIdsRole).toStringList().size();
        if (count > 0) {
            const int titleWidth = p->fontMetrics().horizontalAdvance(title);
            p->setFont(AppStyle::font(AppStyle::Font::Small));
            p->setPen(AppStyle::color(Color::Faint));
            p->drawText(QRectF(textLeft + titleWidth + 8.0, row.top(), 36, row.height()),
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
    if (!layout.disclosure.isNull()) {
        AppIcons::paint(p, i.data(TaskListModel::ChildrenCollapsedRole).toBool()
            ? QStringLiteral("chevron-folder-close") : QStringLiteral("chevron-folder-open"),
            layout.disclosure, AppStyle::color(Color::Muted));
    }

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
                const QString sectionKey = index.data(TaskListModel::SectionKeyRole).toString();
                if (!sectionKey.isEmpty()) {
                    emit sectionToggleActivated(sectionKey);
                    return true;
                }
                return false;
            }
            const TaskLayout layout = taskLayout(option, index);
            if (!layout.disclosure.isNull() && layout.disclosure.contains(mouse->pos())) {
                emit childrenToggleActivated(index.data(TaskListModel::IdRole).toString());
                return true;
            }
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
        const bool organizationEnd = i.data(OrganizationEndRole).toBool();
        const bool after = i.data(DropLineRole).toString() == QLatin1String("after");
        const int y = organizationEnd ? (after ? row.bottom() - 1 : row.top()) : row.top() + 16;
        if (after || !organizationEnd) {
            const QColor color = after ? AppStyle::color(Color::Accent)
                : ThemeManager::instance().color(ds::ColorToken::Divider);
            p->setPen(QPen(color, after ? 2.0 : 1.0));
            p->drawLine(row.left() + 10, y, row.right() - 10, y);
        }
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
    if (selected || hovered) {
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
    if (!folderId.isEmpty()) {
        paintTriangle(p, layout.disclosure.adjusted(2.0, 2.0, -2.0, -2.0),
                      i.data(FolderExpandedRole).toBool(),
                      AppStyle::color(hovered ? Color::Muted : Color::Faint));
    }
    if (o.state & QStyle::State_HasFocus) {
        p->setBrush(Qt::NoBrush);
        p->setPen(QPen(AppStyle::color(Color::Accent), 1.0));
        p->drawRoundedRect(layout.background.adjusted(0.5, 0.5, -0.5, -0.5), radius, radius);
    }
    if (!i.data(DropTargetRole).toString().isEmpty()) {
        p->setBrush(Qt::NoBrush);
        p->setPen(QPen(AppStyle::color(Color::Accent), 1.5));
        p->drawRoundedRect(layout.background.adjusted(0.5, 0.5, -0.5, -0.5), radius, radius);
    }
    const QString dropLine = i.data(DropLineRole).toString();
    if (!dropLine.isEmpty()) {
        const qreal y = dropLine == QLatin1String("after") ? row.bottom() - 1.0 : row.top() + 1.0;
        p->setPen(QPen(AppStyle::color(Color::Accent), 2.0));
        p->drawLine(QPointF(layout.background.left() + 4.0, y),
                    QPointF(layout.background.right() - 4.0, y));
    }
    p->restore();
}

QSize NavigationDelegate::sizeHint(const QStyleOptionViewItem&, const QModelIndex& i) const {
    if (i.data(DividerRole).toBool()) return QSize(160, 33);
    return QSize(160, i.data(SectionRole).toBool() ? 30 : AppStyle::metric(Metric::NavRowHeight));
}

bool NavigationDelegate::editorEvent(QEvent* event, QAbstractItemModel*,
                                     const QStyleOptionViewItem& option,
                                     const QModelIndex& index) {
    if (event->type() != QEvent::MouseButtonRelease) return false;
    auto* mouse = static_cast<QMouseEvent*>(event);
    const QString folderId = index.data(FolderIdRole).toString();
    if (mouse->button() == Qt::LeftButton && !folderId.isEmpty()
        && navigationLayout(option, index).disclosure.contains(mouse->pos())) {
        emit folderToggleActivated(folderId);
        return true;
    }
    return false;
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
