#include "TaskListModel.h"
#include "../app/TaskStore.h"
#include "ListAppearance.h"
#include "Markdown.h"
#include "TaskListView.h"

#include <QMimeData>
#include <QTimeZone>

#include <functional>

namespace kl {
namespace {

QString rowScheduleLabel(const Entity& entity, bool countdown) {
    if (entity.kind != EntityKind::Task) return QString();
    const QDate date = scheduleStartDate(entity), end = scheduleEndDate(entity);
    if (!date.isValid()) return QString();
    const QDate today = toAppTime(appNow()).date();
    if (countdown) {
        if (scheduleIntersects(entity, today, today)) return QStringLiteral("今天");
        const int days = today.daysTo(date >= today ? date : end);
        return days >= 0 ? QStringLiteral("%1天后").arg(days)
                         : QStringLiteral("%1天前").arg(-days);
    }
    QString label;
    if (date == today) label = QStringLiteral("今天");
    else if (date == today.addDays(1)) label = QStringLiteral("明天");
    else if (date == today.addDays(2)) label = QStringLiteral("后天");
    else if (date == today.addDays(-1)) label = QStringLiteral("昨天");
    else if (date.year() == today.year()) label = date.toString(QStringLiteral("M月d日"));
    else label = date.toString(QStringLiteral("yyyy年M月d日"));
    if (entity.hasScheduleRange && end.isValid() && end != date)
        label += QStringLiteral("–") + end.toString(end.year() == date.year()
            ? QStringLiteral("M月d日") : QStringLiteral("yyyy年M月d日"));
    const QDateTime timed = entity.hasScheduleRange ? entity.startAt : entity.dueAt;
    if (timed.isValid()) label += timed.toTimeZone(QTimeZone(entity.timeZone.toUtf8())).toString(QStringLiteral(" HH:mm"));
    return label;
}

QString rowScheduleTooltip(const Entity& entity) {
    if (entity.kind != EntityKind::Task) return QString();
    const QDate date = scheduleStartDate(entity), end = scheduleEndDate(entity);
    if (!date.isValid()) return QString();
    const QDate today = toAppTime(appNow()).date();
    const auto dayLabel = [&today](const QDate& value) {
        const QString weekday = QStringLiteral("周")
            + QStringLiteral("一二三四五六日").mid(value.dayOfWeek() - 1, 1);
        return value.year() == today.year()
            ? weekday + value.toString(QStringLiteral("，M月d日"))
            : value.toString(QStringLiteral("yyyy年M月d日 ")) + weekday;
    };
    QString label = dayLabel(date);
    if (entity.hasScheduleRange && end.isValid() && end != date)
        label += QStringLiteral(" – ") + dayLabel(end);
    const QTimeZone zone(entity.timeZone.toUtf8());
    if (entity.hasScheduleRange && entity.startAt.isValid())
        label += entity.startAt.toTimeZone(zone).toString(QStringLiteral(" HH:mm"))
            + QStringLiteral("–") + entity.dueAt.toTimeZone(zone).toString(QStringLiteral("HH:mm"));
    else if (entity.dueAt.isValid()) label += entity.dueAt.toTimeZone(zone).toString(QStringLiteral(" HH:mm"));
    return label;
}

} // namespace
TaskListModel::TaskListModel(TaskStore* store, QObject* parent) : QAbstractListModel(parent), m_store(store) {
    qRegisterMetaType<GroupKey>();
    connect(store, &TaskStore::taskChanged, this, &TaskListModel::refresh);
    connect(store, &TaskStore::tasksReset, this, &TaskListModel::refresh);
    connect(store, &TaskStore::groupsChanged, this, &TaskListModel::refresh);
    connect(store, &TaskStore::attachmentsChanged, this, &TaskListModel::refresh);
    refresh();
}
int TaskListModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : m_rows.size();
}
QVariant TaskListModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= m_rows.size()) return QVariant();
    const Row& row = m_rows.at(index.row());
    if (row.isHeader) {
        switch (role) {
            case HeaderRole: return true;
            case SectionKeyRole: return row.group.stableId();
            case GroupKeyRole: return QVariant::fromValue(row.group);
            case HeaderEntityIdsRole: return row.sectionEntityIds;
            case SectionCollapsedRole: return m_collapsedSections.contains(row.group.stableId());
            case HeaderActionRole:
                // 已过期桶保留“顺延”批量操作；显式清单与历史视角不提供。
                return row.group.kind == GroupKey::Time && row.group.timeBucket() == TimeBucket::Overdue
                    && m_filter.scope != ViewScope::List
                    && viewClassOf(m_filter.scope) == ViewClass::Active
                    ? QStringLiteral("postpone") : QString();
            case Qt::DisplayRole:
            case Qt::AccessibleTextRole:
                return row.header;
            default:
                return QVariant();
        }
    }
    const Entity& entity = row.entity;
    TaskList list = m_listsById.value(entity.listId);
    const QDate today = toAppTime(appNow()).date();
    switch (role) {
        case Qt::DisplayRole:
            return entity.title;
        case Qt::AccessibleTextRole:
            return entity.title + QStringLiteral("，") +
                (entity.kind == EntityKind::Note ? QStringLiteral("笔记")
                    : (entity.completed ? QStringLiteral("已完成") : QStringLiteral("未完成"))) +
                QStringLiteral("，") + (entity.kind == EntityKind::Task ? scheduleLabel(entity) : QString());
        case Qt::CheckStateRole:
            if (entity.kind == EntityKind::Task) return entity.completed ? Qt::Checked : Qt::Unchecked;
            return QVariant();
        case IdRole:
            return entity.id;
        case KindRole:
            return static_cast<int>(entity.kind);
        case PriorityRole:
            return entity.kind == EntityKind::Task ? entity.priority : 0;
        case CompletedRole:
            return entity.kind == EntityKind::Task && entity.completed;
        case PinnedRole:
            return entity.pinnedAt.isValid();
        case ParentIdRole:
            return entity.parentId;
        case DepthRole:
            return row.depth;
        case HasChildrenRole:
            return row.hasChildren;
        case ChildrenCollapsedRole:
            return row.hasChildren && m_collapsedTasks.contains(entity.id);
        case SectionKeyRole:
            return row.group.stableId();
        case GroupKeyRole:
            return QVariant::fromValue(row.group);
        case ListIdRole:
            return entity.listId;
        case ListNameRole:
            return m_filter.tasksOnly && m_filter.scope == ViewScope::List ? QString() : list.name;
        case ListIconRole:
            return ListAppearance::iconName(list);
        case ListColorRole:
            return list.color;
        case ScheduleRole:
            return rowScheduleLabel(entity, m_filter.countdownDates);
        case ScheduleTooltipRole:
            return rowScheduleTooltip(entity);
        case DetailRole:
            return entity.body.trimmed().isEmpty() ? QString()
                : Markdown::plainText(entity).simplified();
        case DetailsVisibleRole:
            return m_filter.showDetails;
        case ReminderVisibleRole:
            return !entity.reminders.isEmpty();
        case AttachmentVisibleRole:
            return row.hasAttachments;
        case DeletedRole:
            return entity.deleted;
        case OverdueRole: {
            const QDate date = effectiveDate(entity);
            return !entity.deleted && entity.kind == EntityKind::Task && entity.status == TaskStatus::Active && date.isValid() && date < today;
        }
        case HeaderRole:
            return false;
        case MetaRole: {
            QStringList parts;
            if (entity.kind == EntityKind::Note) parts << QStringLiteral("笔记");
            else {
                parts << scheduleLabel(entity);
                if (entity.priority > 0) parts << priorityLabel(entity.priority);
                if (entity.reminderAt.isValid()) parts << QStringLiteral("有提醒");
                if (entity.recurrence != QLatin1String("none")) parts << recurrenceLabel(entity.recurrence);
            }
            if (!list.name.isEmpty()) parts << list.name;
            if (!entity.tags.isEmpty()) parts << QStringLiteral("#") + entity.tags.join(QStringLiteral(" #"));
            return parts.join(QStringLiteral("   "));
        }
        default:
            return QVariant();
    }
}
Qt::ItemFlags TaskListModel::flags(const QModelIndex& index) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= m_rows.size()) return Qt::NoItemFlags;
    const Row& row = m_rows.at(index.row());
    if (row.isHeader) return Qt::ItemIsEnabled | Qt::ItemIsDropEnabled;
    Qt::ItemFlags flags = Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsDropEnabled;
    if (dragBlockReason(index).isEmpty()) flags |= Qt::ItemIsDragEnabled;
    if (row.entity.kind == EntityKind::Task && !row.entity.deleted) flags |= Qt::ItemIsUserCheckable;
    return flags;
}
QStringList TaskListModel::mimeTypes() const {
    return {TaskListView::mimeType()};
}
QMimeData* TaskListModel::mimeData(const QModelIndexList& indexes) const {
    for (const QModelIndex& index : indexes) {
        if (!dragBlockReason(index).isEmpty()) continue;   // 禁拖兜底：不产生 MIME
        const QString id = entityId(index);
        if (id.isEmpty()) continue;
        auto* mime = new QMimeData;
        mime->setData(TaskListView::mimeType(), id.toUtf8());
        mime->setText(index.data(Qt::DisplayRole).toString());
        return mime;
    }
    return nullptr;
}
Qt::DropActions TaskListModel::supportedDragActions() const { return Qt::MoveAction; }
Qt::DropActions TaskListModel::supportedDropActions() const { return Qt::MoveAction; }
bool TaskListModel::setData(const QModelIndex& index, const QVariant& value, int role) {
    const QString id = entityId(index);
    const Entity entity = id.isEmpty() ? Entity() : m_store->entity(id);
    if (role != Qt::CheckStateRole || entity.id.isEmpty() || entity.kind != EntityKind::Task || entity.deleted) return false;
    return m_store->setCompleted(id, value.toInt() == Qt::Checked);
}
void TaskListModel::setFilter(const ViewSpec& filter) {
    m_filter = filter;
    refresh();
}
void TaskListModel::refresh() {
    beginResetModel();
    m_rows.clear();
    m_entityCount = 0;
    m_existingGroups.clear();
    m_listsById.clear();
    for (const TaskList& list : m_store->lists()) m_listsById.insert(list.id, list);
    m_attachmentEntityIds = m_store->attachmentEntityIds();
    const QDate today = toAppTime(appNow()).date();
    const Grouping grouping = groupEntities(m_store->entities(), m_store->lists(), m_filter, today,
                                            m_store->archivedListIds());
    rebuildRows(grouping, today);
    // 拖动中的空组投放区统一追加在末尾：dragEnter 不会在按下位置之前插入标题，
    // 避免目标行坐标在拖动中途变化。
    if (m_dragActive) appendDragTargetHeaders(today);
    endResetModel();
}
void TaskListModel::rebuildRows(const Grouping& grouping, const QDate& today) {
    Q_UNUSED(today);
    struct OrderedGroup {
        QVector<Row> rows;
        QStringList ids;
    };
    QSet<QString> placed;
    std::function<void(OrderedGroup&, const GroupKey&, const Entity&, int, bool)> appendEntity =
        [&](OrderedGroup& group, const GroupKey& sectionKey, const Entity& entity, int depth, bool hidden) {
        if (placed.contains(entity.id)) return;
        placed.insert(entity.id);
        group.ids.append(entity.id);
        if (!hidden) {
            Row row;
            row.entity = entity;
            row.hasAttachments = m_attachmentEntityIds.contains(entity.id);
            row.group = sectionKey;
            row.depth = depth;
            row.hasChildren = grouping.children.contains(entity.id);
            group.rows.append(row);
        }
        const bool hideChildren = hidden || m_collapsedTasks.contains(entity.id);
        for (const Entity& child : grouping.children.value(entity.id))
            appendEntity(group, sectionKey, child, depth + 1, hideChildren);
    };
    for (const GroupedSection& section : grouping.sections) {
        OrderedGroup group;
        for (const Entity& root : section.items) appendEntity(group, section.group, root, 0, false);
        if (group.ids.isEmpty()) continue;
        if (section.group.kind != GroupKey::Flat) {
            Row header;
            header.header = section.title;
            header.group = section.group;
            header.isHeader = true;
            header.sectionEntityIds = group.ids;
            m_rows.append(header);
        }
        m_entityCount += group.ids.size();
        m_existingGroups.insert(section.group.stableId());
        if (section.group.kind == GroupKey::Flat
            || !m_collapsedSections.contains(section.group.stableId()))
            m_rows += group.rows;
    }
}
void TaskListModel::appendDragTargetHeaders(const QDate& today) {
    // 空组投放区：跨组目标 = Time/Priority/List/Pinned（None 用空白区，Created 禁拖）；
    // 笔记源只与置顶互通。终态与 Notes 组不是拖放目标，不物化。
    // 历史视角（Completed/Abandoned/Trash）整体禁止拖放，不物化任何投放区。
    if (viewClassOf(m_filter.scope) == ViewClass::History
        || !groupingAllowsDrag(m_filter.grouping)) return;
    const Entity source = m_store->entity(m_dragSourceId);
    const bool sourceIsNote = !source.id.isEmpty() && source.kind == EntityKind::Note;
    QVector<GroupKey> targets;
    targets.append(GroupKey::pinned());
    if (!sourceIsNote) {
        switch (m_filter.grouping) {
            case GroupMode::Time:
                for (int bucket = 0; bucket <= static_cast<int>(TimeBucket::Undated); ++bucket)
                    targets.append(GroupKey::forTime(static_cast<TimeBucket>(bucket)));
                break;
            case GroupMode::Priority:
                for (int priority = 3; priority >= 0; --priority)
                    targets.append(GroupKey::forPriority(priority));
                break;
            case GroupMode::List:
                for (const TaskList& list : groupingLists(m_store->lists(), m_filter))
                    targets.append(GroupKey::forList(list.id));
                break;
            case GroupMode::Created:
            case GroupMode::None:
                break;
        }
    }
    for (const GroupKey& target : targets) {
        if (m_existingGroups.contains(target.stableId())) continue;
        Row header;
        header.header = target.title(m_store->lists(), today);
        header.group = target;
        header.isHeader = true;
        m_rows.append(header);
        m_existingGroups.insert(target.stableId());
    }
}
void TaskListModel::toggleChildren(const QString& id) {
    if (id.isEmpty()) return;
    if (m_collapsedTasks.contains(id)) m_collapsedTasks.remove(id);
    else m_collapsedTasks.insert(id);
    refresh();
}
void TaskListModel::expandChildren(const QString& id) {
    if (id.isEmpty() || !m_collapsedTasks.contains(id)) return;
    m_collapsedTasks.remove(id);
    refresh();
}
void TaskListModel::toggleSection(const QString& key) {
    if (key.isEmpty()) return;
    if (m_collapsedSections.contains(key)) m_collapsedSections.remove(key);
    else m_collapsedSections.insert(key);
    refresh();
}
void TaskListModel::setDragActive(bool active, const QString& sourceId) {
    const QString dragSourceId = active ? sourceId : QString();
    if (m_dragActive == active && m_dragSourceId == dragSourceId) return;
    m_dragActive = active;
    m_dragSourceId = dragSourceId;
    refresh();
}
QModelIndex TaskListModel::indexForTask(QString id) const {
    for (int i = 0; i < m_rows.size(); ++i)
        if (!m_rows.at(i).isHeader && m_rows.at(i).entity.id == id) return index(i, 0);
    return QModelIndex();
}
QString TaskListModel::entityId(const QModelIndex& index) const {
    return index.isValid() && index.row() >= 0 && index.row() < m_rows.size() && !m_rows.at(index.row()).isHeader
        ? m_rows.at(index.row()).entity.id : QString();
}
GroupKey TaskListModel::groupKeyAt(const QModelIndex& index) const {
    return index.isValid() && index.row() >= 0 && index.row() < m_rows.size()
        ? m_rows.at(index.row()).group : GroupKey();
}
QString TaskListModel::dragBlockReason(const QModelIndex& index) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= m_rows.size())
        return QStringLiteral("当前不能拖动");
    const Row& row = m_rows.at(index.row());
    if (row.isHeader) return QString();   // 段头不是拖动源，无需反馈
    if (viewClassOf(m_filter.scope) == ViewClass::History)
        return QStringLiteral("历史视图不能拖动");   // 历史视角整体禁拖（业务规则不变）
    if (!groupingAllowsDrag(m_filter.grouping))
        return QStringLiteral("按创建时间分组时不能拖动排序");
    if (row.entity.deleted)
        return QStringLiteral("回收站中的项目不能拖动");
    if (row.entity.kind == EntityKind::Task) {
        if (row.entity.status == TaskStatus::Completed)
            return QStringLiteral("已完成的任务不能拖动");
        if (row.entity.status == TaskStatus::Abandoned)
            return QStringLiteral("已放弃的任务不能拖动");
    }
    return QString();
}
}  // namespace kl
