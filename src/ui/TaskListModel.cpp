#include "TaskListModel.h"
#include "../app/TaskStore.h"

namespace kl {
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
        if (role == Qt::DisplayRole || role == Qt::AccessibleTextRole || role == Qt::ToolTipRole) return row.header;
        return QVariant();
    }
    const Entity& entity = row.entity;
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
            for (const TaskList& list : m_store->lists())
                if (list.id == entity.listId) { parts << list.name; break; }
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
    const QVector<ViewSection> sections = buildSections(m_store->entities(), m_filter, toAppTime(appNow()).date());
    for (const ViewSection& section : sections) {
        Row header; header.header = section.title; header.isHeader = true; m_rows.append(header);
        for (const Entity& entity : section.items) {
            Row row; row.entity = entity; m_rows.append(row); ++m_entityCount;
        }
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
