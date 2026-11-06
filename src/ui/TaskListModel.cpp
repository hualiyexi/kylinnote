#include "TaskListModel.h"
#include <algorithm>
#include "../app/TaskStore.h"
namespace kn {
TaskListModel::TaskListModel(TaskStore* store, QObject* parent) : QAbstractListModel(parent), m_store(store) {
    connect(store, &TaskStore::taskChanged, this, &TaskListModel::refresh);
    connect(store, &TaskStore::tasksReset, this, &TaskListModel::refresh);
    connect(store, &TaskStore::groupsChanged, this, &TaskListModel::refresh);
    refresh();
}
int TaskListModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : m_tasks.size();
}
QVariant TaskListModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= m_tasks.size())
        return QVariant();
    const Task& task = m_tasks[index.row()];
    switch (role) {
        case Qt::DisplayRole:
            return task.title;
        case Qt::AccessibleTextRole:
            return task.title + QStringLiteral("，") +
                   (task.completed ? QStringLiteral("已完成") : QStringLiteral("未完成")) +
                   QStringLiteral("，") + scheduleLabel(task);
        case Qt::CheckStateRole:
            return task.completed ? Qt::Checked : Qt::Unchecked;
        case IdRole:
            return task.id;
        case PriorityRole:
            return task.priority;
        case CompletedRole:
            return task.completed;
        case FavoriteRole:
            return task.favorite;
        case PinnedRole:
            return task.pinned;
        case DeletedRole:
            return task.deleted;
        case OverdueRole:
            return !task.completed && task.dueAt.isValid() && task.dueAt < QDateTime::currentDateTime();
        case Qt::ToolTipRole:
            return task.title + QLatin1Char('\n') + scheduleLabel(task);
        case MetaRole: {
            QStringList parts;
            parts << scheduleLabel(task);
            for (const auto& g : m_store->groups())
                if (g.id == task.groupId) {
                    parts << g.name;
                    break;
                }
            if (task.priority > 0)
                parts << priorityLabel(task.priority);
            if (task.reminderAt.isValid())
                parts << QStringLiteral("有提醒");
            if (task.recurrence != QLatin1String("none"))
                parts << recurrenceLabel(task.recurrence);
            if (!task.checklist.isEmpty()) {
                int done = 0;
                for (const auto& c : task.checklist)
                    if (c.done)
                        ++done;
                parts << QStringLiteral("清单 %1/%2").arg(done).arg(task.checklist.size());
            }
            if (task.pinned)
                parts << QStringLiteral("已贴桌面");
            if (!task.tags.isEmpty())
                parts << QStringLiteral("#") + task.tags.join(QStringLiteral(" #"));
            return parts.join(QStringLiteral("   "));
        }
        default:
            return QVariant();
    }
}
Qt::ItemFlags TaskListModel::flags(const QModelIndex& index) const {
    if (!index.isValid())
        return Qt::NoItemFlags;
    Qt::ItemFlags f = Qt::ItemIsEnabled | Qt::ItemIsSelectable;
    if (!data(index, DeletedRole).toBool())
        f |= Qt::ItemIsUserCheckable;
    return f;
}
bool TaskListModel::setData(const QModelIndex& index, const QVariant& value, int role) {
    const qint64 id = taskId(index);
    if (role != Qt::CheckStateRole || !id || m_store->task(id).deleted)
        return false;
    return m_store->setCompleted(id, value.toInt() == Qt::Checked);
}
void TaskListModel::setFilter(const TaskFilter& filter) {
    m_filter = filter;
    refresh();
}
void TaskListModel::refresh() {
    beginResetModel();
    m_tasks.clear();
    const auto now = QDateTime::currentDateTime();
    for (const Task& task : m_store->tasks())
        if (m_filter.matches(task, now))
            m_tasks.append(task);
    std::stable_sort(m_tasks.begin(), m_tasks.end(), [this](const Task& a, const Task& b) {
        return m_filter.lessThan(a, b);
    });
    endResetModel();
}
QModelIndex TaskListModel::indexForTask(qint64 id) const {
    for (int i = 0; i < m_tasks.size(); ++i)
        if (m_tasks[i].id == id)
            return index(i, 0);
    return QModelIndex();
}
qint64 TaskListModel::taskId(const QModelIndex& index) const {
    return index.isValid() && index.row() >= 0 && index.row() < m_tasks.size() ? m_tasks[index.row()].id : 0;
}
}  // namespace kn
