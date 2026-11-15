#include "TaskListModel.h"
#include "../app/TaskStore.h"
#include "ListAppearance.h"
#include "Markdown.h"
#include "TaskListView.h"

#include <QMimeData>

#include <algorithm>
#include <functional>

namespace kl {
namespace {

QString rowScheduleLabel(const Entity& entity, bool countdown) {
    if (entity.kind != EntityKind::Task) return QString();
    const QDate date = effectiveDate(entity);
    if (!date.isValid()) return QString();
    const QDate today = toAppTime(appNow()).date();
    if (countdown) {
        const int days = today.daysTo(date);
        if (days == 0) return QStringLiteral("今天");
        return days > 0 ? QStringLiteral("%1天后").arg(days)
                        : QStringLiteral("%1天前").arg(-days);
    }
    QString label;
    if (date == today) label = QStringLiteral("今天");
    else if (date == today.addDays(1)) label = QStringLiteral("明天");
    else if (date == today.addDays(2)) label = QStringLiteral("后天");
    else if (date == today.addDays(-1)) label = QStringLiteral("昨天");
    else if (date.year() == today.year()) label = date.toString(QStringLiteral("M月d日"));
    else label = date.toString(QStringLiteral("yyyy年M月d日"));
    const QDateTime timed = entity.dueAt.isValid() ? entity.dueAt
        : entity.allDayDate.isValid() ? QDateTime() : entity.startAt;
    if (timed.isValid()) label += toAppTime(timed).toString(QStringLiteral(" HH:mm"));
    return label;
}

QString rowScheduleTooltip(const Entity& entity) {
    const QDate date = effectiveDate(entity);
    if (!date.isValid()) return QString();
    const QDate today = toAppTime(appNow()).date();
    const QString weekday = QStringLiteral("周")
        + QStringLiteral("一二三四五六日").mid(date.dayOfWeek() - 1, 1);
    QString label = date.year() == today.year()
        ? weekday + date.toString(QStringLiteral("，M月d日"))
        : date.toString(QStringLiteral("yyyy年M月d日 ")) + weekday;
    const QDateTime timed = entity.dueAt.isValid() ? entity.dueAt
        : entity.allDayDate.isValid() ? QDateTime() : entity.startAt;
    if (timed.isValid()) label += toAppTime(timed).toString(QStringLiteral(" HH:mm"));
    return label;
}

}  // namespace
TaskListModel::TaskListModel(TaskStore* store, QObject* parent) : QAbstractListModel(parent), m_store(store) {
    connect(store, &TaskStore::taskChanged, this, &TaskListModel::refresh);
    connect(store, &TaskStore::tasksReset, this, &TaskListModel::refresh);
    connect(store, &TaskStore::groupsChanged, this, &TaskListModel::refresh);
    refresh();
}
int TaskListModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : m_rows.size();
}
QVariant TaskListModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= m_rows.size()) return QVariant();
    const Row& row = m_rows.at(index.row());
    if (row.isHeader) {
        if (role == HeaderRole) return true;
        if (role == SectionKeyRole) return row.sectionKey;
        if (role == HeaderEntityIdsRole) return row.sectionEntityIds;
        if (role == SectionCollapsedRole) return m_collapsedSections.contains(row.sectionKey);
        if (role == HeaderActionRole) {
            return row.sectionKey == QLatin1String("overdue") && m_filter.scope != ViewScope::List
                ? QStringLiteral("postpone") : QString();
        }
        if (role == Qt::DisplayRole || role == Qt::AccessibleTextRole) return row.header;
        return QVariant();
    }
    const Entity& entity = row.entity;
    TaskList list;
    for (const TaskList& candidate : m_store->lists()) {
        if (candidate.id == entity.listId) {
            list = candidate;
            break;
        }
    }
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
            return row.sectionKey;
        case UnpinnedSectionRole:
            return unpinnedSection(entity);
        case ListIdRole:
            return entity.listId;
        case ListNameRole:
            return list.name;
        case ListIconRole:
            return ListAppearance::iconName(list);
        case ListColorRole:
            return list.color;
        case ScheduleRole:
            return rowScheduleLabel(entity, m_filter.countdownDates);
        case ScheduleTooltipRole:
            return rowScheduleTooltip(entity);
        case DetailRole: {
            QStringList parts;
            if (!entity.body.trimmed().isEmpty())
                parts.append(Markdown::plainText(entity).simplified());
            if (!entity.checklist.isEmpty()) {
                int done = 0;
                for (const ChecklistItem& item : entity.checklist) if (item.done) ++done;
                parts.append(QStringLiteral("检查项 %1/%2").arg(done).arg(entity.checklist.size()));
            }
            return parts.join(QStringLiteral(" · "));
        }
        case DetailsVisibleRole:
            return m_filter.showDetails;
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
            if (!entity.checklist.isEmpty()) {
                int done = 0;
                for (const ChecklistItem& item : entity.checklist) if (item.done) ++done;
                parts << QStringLiteral("清单 %1/%2").arg(done).arg(entity.checklist.size());
            }
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
    const bool active = !row.entity.deleted
        && (row.entity.kind == EntityKind::Note || row.entity.status == TaskStatus::Active);
    if (active) flags |= Qt::ItemIsDragEnabled;
    if (row.entity.kind == EntityKind::Task && !row.entity.deleted) flags |= Qt::ItemIsUserCheckable;
    return flags;
}
QStringList TaskListModel::mimeTypes() const {
    return {TaskListView::mimeType()};
}
QMimeData* TaskListModel::mimeData(const QModelIndexList& indexes) const {
    for (const QModelIndex& index : indexes) {
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
    m_rows.clear(); m_entityCount = 0;
    const QVector<ViewSection> sections = m_filter.scope == ViewScope::Folder
        ? buildFolderSections(m_store->entities(), m_store->lists(), m_filter,
                              toAppTime(appNow()).date(), m_store->archivedListIds())
        : buildSections(m_store->entities(), m_filter, toAppTime(appNow()).date(),
                        m_store->archivedListIds());
    const QDate today = toAppTime(appNow()).date();
    QVector<ViewSection> displaySections;
    for (const ViewSection& section : sections) {
        if (m_filter.renderer != ViewRenderer::Time || section.key != QLatin1String("upcoming")) {
            displaySections.append(section);
            continue;
        }
        QVector<Entity> todayItems, tomorrowItems, next7Items, otherItems;
        for (const Entity& entity : section.items) {
            const QDate date = effectiveDate(entity);
            if (date == today) todayItems.append(entity);
            else if (date == today.addDays(1)) tomorrowItems.append(entity);
            else if (date > today.addDays(1) && date <= today.addDays(6)) next7Items.append(entity);
            else otherItems.append(entity);
        }
        const auto weekTitle = [](const QDate& date) {
            return QStringLiteral("周") + QStringLiteral("一二三四五六日").mid(date.dayOfWeek() - 1, 1);
        };
        if (!todayItems.isEmpty())
            displaySections.append({QStringLiteral("today"), QStringLiteral("今天 ") + weekTitle(today), todayItems});
        if (!tomorrowItems.isEmpty())
            displaySections.append({QStringLiteral("tomorrow"), QStringLiteral("明天 ") + weekTitle(today.addDays(1)), tomorrowItems});
        if (!next7Items.isEmpty())
            displaySections.append({QStringLiteral("next7days"), QStringLiteral("最近7天"), next7Items});
        if (!otherItems.isEmpty())
            displaySections.append({QStringLiteral("upcoming"), section.title, otherItems});
    }

    QVector<ViewSection> dropTargets;
    if (m_dragActive) {
        dropTargets.append({QStringLiteral("pinned"), QStringLiteral("已置顶"), {}});
        if (m_filter.renderer == ViewRenderer::Time) {
            const auto weekTitle = [](const QDate& date) {
                return QStringLiteral("周") + QStringLiteral("一二三四五六日").mid(date.dayOfWeek() - 1, 1);
            };
            if (m_filter.scope == ViewScope::Today || m_filter.scope == ViewScope::Next7Days
                || m_filter.scope == ViewScope::Overdue)
                dropTargets.append({QStringLiteral("overdue"), QStringLiteral("已过期"), {}});
            if (m_filter.scope == ViewScope::Today || m_filter.scope == ViewScope::Next7Days)
                dropTargets.append({QStringLiteral("today"), QStringLiteral("今天 ") + weekTitle(today), {}});
            if (m_filter.scope == ViewScope::Tomorrow || m_filter.scope == ViewScope::Next7Days)
                dropTargets.append({QStringLiteral("tomorrow"), QStringLiteral("明天 ") + weekTitle(today.addDays(1)), {}});
            if (m_filter.scope == ViewScope::Next7Days)
                dropTargets.append({QStringLiteral("next7days"), QStringLiteral("最近7天"), {}});
        } else {
            const Entity source = m_store->entity(m_dragSourceId);
            if (!source.id.isEmpty()) {
                const QString key = unpinnedSection(source);
                QString title;
                if (key == QLatin1String("overdue")) title = QStringLiteral("已过期");
                else if (key == QLatin1String("upcoming")) title = QStringLiteral("未过期");
                else if (key == QLatin1String("undated")) title = QStringLiteral("无日期");
                else if (key == QLatin1String("notes")) title = QStringLiteral("笔记");
                else {
                    for (const TaskList& list : m_store->lists())
                        if (list.id == source.listId) { title = list.name; break; }
                }
                dropTargets.append({key, title, {}});
            }
        }
    }

    QHash<QString, const Entity*> visible;
    QHash<QString, int> originalOrder;
    int order = 0;
    for (const ViewSection& section : displaySections) {
        for (const Entity& entity : section.items) {
            visible.insert(entity.id, &entity);
            originalOrder.insert(entity.id, order++);
        }
    }
    QHash<QString, QVector<Entity>> children;
    QSet<QString> nested;
    for (const ViewSection& section : displaySections) {
        for (const Entity& entity : section.items) {
            const Entity* parent = visible.value(entity.parentId, nullptr);
            if (!parent || parent->listId != entity.listId) continue;
            children[entity.parentId].append(entity);
            nested.insert(entity.id);
        }
    }
    for (auto it = children.begin(); it != children.end(); ++it) {
        std::sort(it->begin(), it->end(), [&originalOrder](const Entity& a, const Entity& b) {
            if (a.sortOrder != b.sortOrder) return a.sortOrder < b.sortOrder;
            return originalOrder.value(a.id) < originalOrder.value(b.id);
        });
    }

    struct OrderedSection {
        QVector<Row> rows;
        QStringList ids;
    };
    QVector<OrderedSection> grouped(displaySections.size());
    QSet<QString> placed;
    std::function<void(int, const Entity&, int, bool)> appendEntity;
    appendEntity = [&](int sectionIndex, const Entity& entity, int depth, bool hidden) {
        if (placed.contains(entity.id)) return;
        placed.insert(entity.id);
        OrderedSection& group = grouped[sectionIndex];
        group.ids.append(entity.id);
        if (!hidden) {
            Row row;
            row.entity = entity;
            row.sectionKey = displaySections.at(sectionIndex).key;
            row.depth = depth;
            row.hasChildren = children.contains(entity.id);
            group.rows.append(row);
        }
        const bool hideChildren = hidden || m_collapsedTasks.contains(entity.id);
        for (const Entity& child : children.value(entity.id))
            appendEntity(sectionIndex, child, depth + 1, hideChildren);
    };
    for (int index = 0; index < displaySections.size(); ++index) {
        for (const Entity& entity : displaySections.at(index).items)
            if (!nested.contains(entity.id)) appendEntity(index, entity, 0, false);
    }
    // 正常子树先跨全部分区放置；仅循环等异常旧数据还会遗漏，统一在最后兜底。
    for (int index = 0; index < displaySections.size(); ++index) {
        for (const Entity& entity : displaySections.at(index).items)
            if (!placed.contains(entity.id)) appendEntity(index, entity, 0, false);
    }
    for (int index = 0; index < displaySections.size(); ++index) {
        const ViewSection& section = displaySections.at(index);
        const OrderedSection& group = grouped.at(index);
        if (group.ids.isEmpty()) continue;
        Row header;
        header.header = section.key == QLatin1String("pinned") ? QStringLiteral("已置顶") : section.title;
        header.sectionKey = section.key;
        header.isHeader = true;
        header.sectionEntityIds = group.ids;
        m_rows.append(header);
        m_entityCount += group.ids.size();
        if (!m_collapsedSections.contains(section.key))
            m_rows += group.rows;
    }
    // 父子归组后原分区可能变空。先输出所有正常行，再统一追加临时投放区，
    // 这样 dragEnter 不会在按下位置之前插入标题并改变目标坐标。
    for (const ViewSection& target : dropTargets) {
        bool found = false;
        for (int index = 0; index < displaySections.size(); ++index) {
            if (displaySections.at(index).key == target.key && !grouped.at(index).ids.isEmpty()) {
                found = true;
                break;
            }
        }
        if (found) continue;
        Row header;
        header.header = target.key == QLatin1String("pinned") ? QStringLiteral("已置顶") : target.title;
        header.sectionKey = target.key;
        header.isHeader = true;
        m_rows.append(header);
    }
    endResetModel();
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
QString TaskListModel::unpinnedSection(const Entity& entity) const {
    if (m_filter.scope == ViewScope::Folder) return QStringLiteral("list:") + entity.listId;
    if (entity.kind == EntityKind::Note) return QStringLiteral("notes");
    const QDate date = effectiveDate(entity);
    const QDate today = toAppTime(appNow()).date();
    if (!date.isValid()) return QStringLiteral("undated");
    if (date < today) return QStringLiteral("overdue");
    if (m_filter.renderer == ViewRenderer::Time) {
        if (date == today) return QStringLiteral("today");
        if (date == today.addDays(1)) return QStringLiteral("tomorrow");
        if (date <= today.addDays(6)) return QStringLiteral("next7days");
    }
    return QStringLiteral("upcoming");
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
}  // namespace kl
