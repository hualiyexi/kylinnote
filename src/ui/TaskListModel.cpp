#include "TaskListModel.h"
#include "../app/TaskStore.h"
#include "ListAppearance.h"

namespace kl {
namespace {

QString rowScheduleLabel(const Entity& entity) {
    if (entity.kind != EntityKind::Task) return QString();
    const QDate date = effectiveDate(entity);
    if (!date.isValid()) return QString();
    const QDate today = toAppTime(appNow()).date();
    QString label;
    if (date == today) label = QStringLiteral("今天");
    else if (date == today.addDays(1)) label = QStringLiteral("明天");
    else if (date == today.addDays(2)) label = QStringLiteral("后天");
    else if (date == today.addDays(-1)) label = QStringLiteral("昨天");
    else if (date.year() == today.year()) label = date.toString(QStringLiteral("M月d日"));
    else label = date.toString(QStringLiteral("yyyy年M月d日"));
    const QDateTime timed = entity.dueAt.isValid() ? entity.dueAt : entity.startAt;
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
        if (role == HeaderActionRole) {
            return row.sectionKey == QLatin1String("overdue") && m_filter.scope != ViewScope::List
                ? QStringLiteral("postpone") : QString();
        }
        if (role == Qt::DisplayRole || role == Qt::AccessibleTextRole || role == Qt::ToolTipRole) return row.header;
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
        case ListIdRole:
            return entity.listId;
        case ListNameRole:
            return list.name;
        case ListIconRole:
            return ListAppearance::iconName(list);
        case ListColorRole:
            return list.color;
        case ScheduleRole:
            return rowScheduleLabel(entity);
        case ScheduleTooltipRole:
            return entity.allDayDate.isValid() && !entity.dueAt.isValid()
                ? entity.allDayDate.toString(QStringLiteral("yyyy年M月d日")) : scheduleLabel(entity);
        case DeletedRole:
            return entity.deleted;
        case OverdueRole: {
            const QDate date = effectiveDate(entity);
            return !entity.deleted && entity.kind == EntityKind::Task && entity.status == TaskStatus::Active && date.isValid() && date < today;
        }
        case HeaderRole:
            return false;
        case Qt::ToolTipRole:
            return entity.title + QLatin1Char('\n') + (entity.kind == EntityKind::Note ? QStringLiteral("笔记") : scheduleLabel(entity));
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
    if (row.isHeader) return Qt::ItemIsEnabled;
    Qt::ItemFlags flags = Qt::ItemIsEnabled | Qt::ItemIsSelectable;
    if (row.entity.kind == EntityKind::Task && !row.entity.deleted) flags |= Qt::ItemIsUserCheckable;
    return flags;
}
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
    const QVector<ViewSection> sections = buildSections(
        m_store->entities(), m_filter, toAppTime(appNow()).date(), m_store->archivedListIds());
    const auto appendRows = [this](const QString& key, const QString& title,
                                    const QVector<Entity>& items) {
        Row header;
        header.header = key == QLatin1String("pinned") ? QStringLiteral("已置顶") : title;
        header.sectionKey = key;
        for (const Entity& entity : items) header.sectionEntityIds.append(entity.id);
        header.isHeader = true;
        m_rows.append(header);
        for (const Entity& entity : items) {
            Row row; row.entity = entity; m_rows.append(row); ++m_entityCount;
        }
    };
    const QDate today = toAppTime(appNow()).date();
    for (const ViewSection& section : sections) {
        if (m_filter.renderer != ViewRenderer::Time || section.key != QLatin1String("upcoming")) {
            appendRows(section.key, section.title, section.items);
            continue;
        }
        QDate date;
        QVector<Entity> dayItems;
        const auto flushDay = [&]() {
            if (dayItems.isEmpty()) return;
            const QString weekday = QStringLiteral("一二三四五六日").mid(date.dayOfWeek() - 1, 1);
            const QDate currentWeek = today.addDays(1 - today.dayOfWeek());
            const QString week = date >= currentWeek.addDays(7)
                ? QStringLiteral("下周") + weekday : QStringLiteral("周") + weekday;
            const QString title = date == today ? QStringLiteral("今天 ") + week
                : date.toString(QStringLiteral("M月d日 ")) + week;
            appendRows(QStringLiteral("date:") + date.toString(Qt::ISODate), title, dayItems);
        };
        for (const Entity& entity : section.items) {
            const QDate next = effectiveDate(entity);
            if (date != next) {
                flushDay();
                dayItems.clear();
                date = next;
            }
            dayItems.append(entity);
        }
        flushDay();
    }
    endResetModel();
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
